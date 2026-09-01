/**
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import React, { useState, useEffect, useRef, useCallback } from "react"
import LanguageLane from "./components/LanguageLane"
import ResponseDrawer from "./components/ResponseDrawer"
import Visualizer from "./components/Visualizer"
import { useAudioRecorder } from "./hooks/useAudioRecorder"
import {
  transcribeAudio,
  translateText,
  splitTextIntoSpeechChunks,
} from "./utils/api"
import { playBlip } from "./utils/audio-blip"

// Core orchestrator for the two-person kiosk translator.
// Flow: hold a key → record mic (useAudioRecorder) → POST /api/stt (Moonshine)
// → LLM translation via /proxy (Gemma, strict-JSON prompt) → /api/tts playback.

// Languages offered on each lane's revolver; ttsLang selects the backend voice.
const AVAILABLE_LANGUAGES = [
  { code: "zh", name: "Chinese", voice: "tts", ttsLang: "zh" },
  { code: "en", name: "English", voice: "tts", ttsLang: "en" },
  { code: "ar", name: "Arabic", voice: "tts", ttsLang: "ar" },
  { code: "es", name: "Spanish", voice: "tts", ttsLang: "es" },
  { code: "ja", name: "Japanese", voice: "tts", ttsLang: "ja" },
  { code: "ko", name: "Korean", voice: "tts", ttsLang: "ko" },
]

const languageIndexFor = (code, fallbackIndex) => {
  const index = AVAILABLE_LANGUAGES.findIndex((language) => language.code === code)
  return index >= 0 ? index : fallbackIndex
}

function TranslatorApp({ config, setConfig }) {
  // UI State
  const [isDrawerOpen, setIsDrawerOpen] = useState(false)
  const [activePerson, setActivePerson] = useState(1)

  // Translation State
  const [transcriptionData, setTranscriptionData] = useState({
    source: "",
    text: "— listening —",
  })
  const [translationData, setTranslationData] = useState({
    target: "",
    text: "— waiting —",
  })
  const [metaText, setMetaText] = useState("")
  const [timing, setTiming] = useState(null)

  // Currently-playing TTS audio element (chunked playback chain)
  const onlineAudioPlayerRef = useRef(null)

  // Language Lanes State
  const [lang1Index, setLang1Index] = useState(() =>
    languageIndexFor(config.lane1Language, 0),
  )
  const [lang2Index, setLang2Index] = useState(() =>
    languageIndexFor(config.lane2Language, 1),
  )
  const [activeLaneRecording, setActiveLaneRecording] = useState(null) // 1 or 2
  const activeLaneRecordingRef = useRef(null)
  const hardwareButtonPressedRef = useRef(false)
  const lang1IndexRef = useRef(lang1Index)
  const lang2IndexRef = useRef(lang2Index)
  const configRef = useRef(config)

  const { isRecording, startRecording, stopRecording, analyser, micError } =
    useAudioRecorder()

  useEffect(() => {
    if (micError) {
      setIsDrawerOpen(true)
      setTranscriptionData({ source: "Microphone", text: "Access Failed" })
      setTranslationData({
        target: "Error",
        text: `${micError} (HTTPS is required when accessing from remote devices)`,
      })
    }
  }, [micError])

  useEffect(() => {
    setLang1Index(languageIndexFor(config.lane1Language, 0))
  }, [config.lane1Language])

  useEffect(() => {
    setLang2Index(languageIndexFor(config.lane2Language, 1))
  }, [config.lane2Language])

  useEffect(() => {
    lang1IndexRef.current = lang1Index
  }, [lang1Index])

  useEffect(() => {
    lang2IndexRef.current = lang2Index
  }, [lang2Index])

  useEffect(() => {
    configRef.current = config
  }, [config])

  useEffect(() => {
    const preventContextMenu = (e) => e.preventDefault()
    window.addEventListener("contextmenu", preventContextMenu)
    return () => window.removeEventListener("contextmenu", preventContextMenu)
  }, [])

  const stopSpeaking = useCallback(() => {
    if (onlineAudioPlayerRef.current) {
      onlineAudioPlayerRef.current.pause()
      if (onlineAudioPlayerRef.current.dataset.objectUrl) {
        URL.revokeObjectURL(onlineAudioPlayerRef.current.dataset.objectUrl)
      }
      onlineAudioPlayerRef.current = null
    }
  }, [])

  // Speak text via /api/tts, splitting into ~180-char chunks and chaining
  // playback so long translations don't overflow a single TTS request.
  const playTTS = useCallback(
    async (text, targetLang, onSynthesisReady) => {
      if (!text) return 0
      stopSpeaking()

      const chunks = splitTextIntoSpeechChunks(text)
      if (chunks.length === 0) return 0

      let chunkIndex = 0
      let synthesisMs = 0

      return new Promise((resolve, reject) => {
        const playNextChunk = async () => {
          if (chunkIndex >= chunks.length) {
            stopSpeaking()
            resolve(synthesisMs)
            return
          }

          const ttsUrl = `/api/tts?text=${encodeURIComponent(chunks[chunkIndex])}&lang=${encodeURIComponent(targetLang)}`
          const chunkStart = performance.now()

          let objectUrl = null
          try {
            const response = await fetch(ttsUrl, { cache: "no-store" })
            if (!response.ok) throw new Error(`TTS failed: ${response.status}`)
            const audioBlob = await response.blob()
            synthesisMs += performance.now() - chunkStart
            onSynthesisReady?.(synthesisMs)

            objectUrl = URL.createObjectURL(audioBlob)
            const player = new Audio(objectUrl)
            player.volume = 1.0
            player.dataset.objectUrl = objectUrl
            onlineAudioPlayerRef.current = player

            player.onended = () => {
              URL.revokeObjectURL(objectUrl)
              if (onlineAudioPlayerRef.current === player) {
                onlineAudioPlayerRef.current = null
              }
              chunkIndex++
              playNextChunk()
            }
            player.onerror = () => {
              URL.revokeObjectURL(objectUrl)
              stopSpeaking()
              alert("TTS playback failed. Backend server may be offline.")
              reject(new Error("TTS playback failed"))
            }
            player.play().catch((e) => {
              URL.revokeObjectURL(objectUrl)
              console.error("Audio play error:", e)
              stopSpeaking()
              reject(e)
            })
          } catch (e) {
            if (objectUrl) URL.revokeObjectURL(objectUrl)
            stopSpeaking()
            reject(e)
          }
        }

        playNextChunk()
      })
    },
    [stopSpeaking],
  )

  // Rotate a lane's language, skipping the slot held by the other lane
  // (the two lanes may never show the same language).
  const handleRotateLanguage = useCallback(
    (lane, direction) => {
      if (isRecording) return
      const N = AVAILABLE_LANGUAGES.length

      playBlip("language")

      if (lane === 1) {
        let ni = (lang1Index + direction + N) % N
        if (ni === lang2Index) ni = (ni + direction + N) % N
        setLang1Index(ni)
        lang1IndexRef.current = ni
        configRef.current = {
          ...configRef.current,
          lane1Language: AVAILABLE_LANGUAGES[ni].code,
        }
        setConfig?.((prev) => ({ ...prev, lane1Language: AVAILABLE_LANGUAGES[ni].code }))
      } else {
        let ni = (lang2Index + direction + N) % N
        if (ni === lang1Index) ni = (ni + direction + N) % N
        setLang2Index(ni)
        lang2IndexRef.current = ni
        configRef.current = {
          ...configRef.current,
          lane2Language: AVAILABLE_LANGUAGES[ni].code,
        }
        setConfig?.((prev) => ({ ...prev, lane2Language: AVAILABLE_LANGUAGES[ni].code }))
      }
    },
    [lang1Index, lang2Index, isRecording, setConfig],
  )

  // Recording triggers
  const handleRecordStart = useCallback(
    async (lane) => {
      stopSpeaking()

      setActivePerson((prev) => {
        if (prev !== lane) playBlip("speaker")
        return lane
      })

      const ok = await startRecording()
      if (ok) {
        activeLaneRecordingRef.current = lane
        setActiveLaneRecording(lane)
        playBlip("ping")
      } else {
        activeLaneRecordingRef.current = null
        setActiveLaneRecording(null)
      }
    },
    [stopSpeaking, startRecording],
  )

  const handleRecordStop = useCallback(async () => {
    const recordedLane = activeLaneRecordingRef.current
    activeLaneRecordingRef.current = null
    setActiveLaneRecording(null)
    const audioData = await stopRecording()

    if (audioData && recordedLane) {
      processTranslation(recordedLane, audioData.base64Data)
    }
  }, [stopRecording])

  // Translation Pipeline
  const processTranslation = async (lane, base64Data) => {
    setIsDrawerOpen(true)

    const currentConfig = configRef.current
    const lane1Language =
      AVAILABLE_LANGUAGES[
      languageIndexFor(currentConfig.lane1Language, lang1IndexRef.current)
      ]
    const lane2Language =
      AVAILABLE_LANGUAGES[
      languageIndexFor(currentConfig.lane2Language, lang2IndexRef.current)
      ]
    const src =
      lane === 1
        ? lane1Language
        : lane2Language
    const dst =
      lane === 1
        ? lane2Language
        : lane1Language

    setTranscriptionData({
      source: `${src.name} (Source)`,
      text: "Analyzing voice input...",
    })
    setTranslationData({
      target: `${dst.name} (Translation)`,
      text: "Translatng",
    })
    setMetaText("")
    setTiming({
      stt: "loading",
      translate: null,
      tts: currentConfig.enableTts ? null : "off",
    })

    try {
      // 1. Transcription
      setTranscriptionData((prev) => ({ ...prev, text: "Listening..." }))
      const sttStart = performance.now()
      const transcribedText = await transcribeAudio(base64Data, src.code)
      const sttSeconds = (performance.now() - sttStart) / 1000
      setTiming((prev) => ({ ...prev, stt: sttSeconds, translate: "loading" }))
      setTranscriptionData((prev) => ({ ...prev, text: transcribedText }))

      if (!transcribedText.trim()) {
        setTranslationData((prev) => ({
          ...prev,
          text: "(No speech detected)",
        }))
        setTiming((prev) => ({ ...prev, translate: null, tts: null }))
        return
      }

      // 2. Translation
      const result = await translateText(transcribedText, {
        ...currentConfig,
        modelName: currentConfig.modelName,
        systemPrompt: `You are a high-performance translator. Your task is to translate text from ${src.name.split(" ")[0]} into ${dst.name.split(" ")[0]}.\nYou MUST format your response as a valid JSON object matching this structure:\n{\n  "translation": "High-quality, natural translation into ${dst.name.split(" ")[0]}"\n}\nDo NOT return anything else except this JSON object. No Markdown block wraps (no \`\`\`json), no introductory text, no conversational text. Start directly with "{" and end directly with "}".`,
      })

      setTranslationData((prev) => ({ ...prev, text: result.translation }))
      setTiming((prev) => ({
        ...prev,
        translate: Number(result.duration),
        tts: currentConfig.enableTts ? "loading" : "off",
      }))
      setMetaText(`Tokens: ${result.tokens}`)

      if (currentConfig.enableTts) {
        playTTS(result.translation, dst.ttsLang, (ttsMs) => {
          setTiming((prev) => ({ ...prev, tts: ttsMs / 1000 }))
        }).catch((err) => {
          console.error(err)
          setTiming((prev) => ({ ...prev, tts: "error" }))
        })
      }
    } catch (err) {
      console.error(err)
      setTranscriptionData((prev) => ({
        ...prev,
        text: prev.text === "Listening..." ? "(Transcription failed)" : prev.text,
      }))
      setTranslationData((prev) => ({ ...prev, text: `Error: ${err.message}` }))
      setTiming((prev) => {
        if (!prev) return prev
        return {
          stt: prev.stt === "loading" ? "error" : prev.stt,
          translate: prev.translate === "loading" ? "error" : prev.translate,
          tts: prev.tts === "loading" ? "error" : prev.tts,
        }
      })
    }
  }

  // Push-to-talk keyboard control (two modes, see README):
  // landscape = one "active person" driven by Space/Z/arrows;
  // vertical   = independent per-lane keys (Z/X for record, arrows and -/+).
  // keydown starts recording, keyup stops — e.repeat guards auto-repeat.
  useEffect(() => {
    const handleKeyDown = (e) => {
      if (["INPUT", "TEXTAREA", "SELECT"].includes(e.target.tagName)) return
      const key = e.key.toLowerCase()

      if (config.keyboardMode === "landscape") {
        if (key === " " || e.key === "Spacebar") {
          e.preventDefault()
          if (!isRecording) {
            playBlip("speaker")
            setActivePerson((p) => (p === 1 ? 2 : 1))
          }
        } else if (key === "z") {
          e.preventDefault()
          if (!e.repeat && !isRecording) handleRecordStart(activePerson)
        } else if (e.key === "ArrowLeft") {
          e.preventDefault()
          handleRotateLanguage(activePerson, -1)
        } else if (e.key === "ArrowRight") {
          e.preventDefault()
          handleRotateLanguage(activePerson, 1)
        }
      } else {
        if (key === "z") {
          e.preventDefault()
          if (!e.repeat && !isRecording) handleRecordStart(1)
        } else if (key === "x") {
          e.preventDefault()
          if (!e.repeat && !isRecording) handleRecordStart(2)
        } else if (e.key === "ArrowLeft") {
          e.preventDefault()
          handleRotateLanguage(1, -1)
        } else if (e.key === "ArrowRight") {
          e.preventDefault()
          handleRotateLanguage(1, 1)
        } else if (key === "-" || key === "_") {
          e.preventDefault()
          handleRotateLanguage(2, -1)
        } else if (key === "+" || key === "=") {
          e.preventDefault()
          handleRotateLanguage(2, 1)
        }
      }
    }

    const handleKeyUp = (e) => {
      if (["INPUT", "TEXTAREA", "SELECT"].includes(e.target.tagName)) return
      const key = e.key.toLowerCase()

      if (config.keyboardMode === "landscape") {
        if (key === "z" && isRecording) handleRecordStop()
      } else {
        if (key === "z" && activeLaneRecording === 1) handleRecordStop()
        if (key === "x" && activeLaneRecording === 2) handleRecordStop()
      }
    }

    window.addEventListener("keydown", handleKeyDown)
    window.addEventListener("keyup", handleKeyUp)
    return () => {
      window.removeEventListener("keydown", handleKeyDown)
      window.removeEventListener("keyup", handleKeyUp)
    }
  }, [
    config.keyboardMode,
    isRecording,
    activePerson,
    activeLaneRecording,
    handleRecordStart,
    handleRecordStop,
    handleRotateLanguage,
  ])

  // whisplay-plus hardware button support. The GPIO bridge writes button state
  // for backend/server.py, and the kiosk UI translates it into the same
  // push-to-talk actions used by the keyboard path.
  useEffect(() => {
    let cancelled = false
    let unavailableCount = 0

    const pollHardware = async () => {
      try {
        const res = await fetch("/api/hardware", { cache: "no-store" })
        if (!res.ok) throw new Error(`hardware status ${res.status}`)
        const state = await res.json()
        if (!state.enabled || state.stale) return

        unavailableCount = 0
        const pressed = Boolean(state.buttonPressed)
        if (pressed !== hardwareButtonPressedRef.current) {
          hardwareButtonPressedRef.current = pressed
          if (pressed) {
            handleRecordStart(activePerson)
          } else {
            handleRecordStop()
          }
        }
      } catch (err) {
        unavailableCount += 1
        if (unavailableCount === 1) {
          console.debug("Hardware controls unavailable:", err)
        }
      } finally {
        if (!cancelled) window.setTimeout(pollHardware, unavailableCount > 10 ? 1000 : 80)
      }
    }

    pollHardware()
    return () => {
      cancelled = true
    }
  }, [activePerson, handleRecordStart, handleRecordStop])

  return (
    <div className="translator-envelope">
      <ResponseDrawer
        isActive={isDrawerOpen}
        onClose={() => setIsDrawerOpen(false)}
        transcriptionSource={transcriptionData.source}
        transcriptionText={transcriptionData.text}
        translationTarget={translationData.target}
        translationText={translationData.text}
        metaText={metaText}
        timing={timing}
      />

      <main className="translator-workspace">
        <Visualizer
          activePerson={activePerson}
          isRecording={isRecording}
          analyser={analyser}
          barsCount={parseInt(config.visualizerBars, 10)}
        />

        <div className="languages-container">
          <LanguageLane
            laneId={1}
            laneLabel="1"
            languages={AVAILABLE_LANGUAGES}
            currentIndex={lang1Index}
            isRecording={activeLaneRecording === 1}
            isActivePerson={activePerson === 1}
            onRotate={(dir) => handleRotateLanguage(1, dir)}
            onSelect={() => setActivePerson(1)}
            onPressStart={() => handleRecordStart(1)}
            onPressEnd={handleRecordStop}
          />
          <LanguageLane
            laneId={2}
            laneLabel="2"
            languages={AVAILABLE_LANGUAGES}
            currentIndex={lang2Index}
            isRecording={activeLaneRecording === 2}
            isActivePerson={activePerson === 2}
            onRotate={(dir) => handleRotateLanguage(2, dir)}
            onSelect={() => setActivePerson(2)}
            onPressStart={() => handleRecordStart(2)}
            onPressEnd={handleRecordStop}
          />
        </div>
      </main>
    </div>
  )
}

export default TranslatorApp
