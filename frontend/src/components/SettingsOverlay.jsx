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

import React from "react"
import { playBlip } from "../utils/audio-blip"

// Fullscreen developer settings: theme color, LLM endpoint/model/key,
// keyboard mode, TTS toggle, visualizer density, and system volume
// (proxied to amixer on the Pi via the backend's /api/volume).
export default function SettingsOverlay({
  isActive,
  onClose,
  config,
  setConfig,
  onTestConnection,
}) {
  const LANGUAGE_OPTIONS = [
    { code: "zh", name: "Chinese" },
    { code: "en", name: "English" },
    { code: "ar", name: "Arabic" },
    { code: "es", name: "Spanish" },
    { code: "ja", name: "Japanese" },
    { code: "ko", name: "Korean" },
  ];

  const THEME_COLORS = [
    { name: "RED", value: "#ff4444" },
    { name: "WHITE", value: "#ffffff" },
    { name: "YELLOW", value: "#ffeb3b" },
    { name: "BLUE", value: "#2196f3" },
    { name: "GREEN", value: "#4caf50" },
    { name: "ORANGE", value: "#ffa500" },
  ];

  const [systemVolume, setSystemVolume] = React.useState(null);
  const [languageStatus, setLanguageStatus] = React.useState({});

  const refreshLanguageStatus = React.useCallback(async () => {
    try {
      const res = await fetch('/api/languages', { cache: 'no-store' });
      const data = await res.json();
      const nextStatus = {};
      for (const language of data.languages || []) {
        nextStatus[language.code] = language.status;
      }
      setLanguageStatus(nextStatus);
    } catch (e) {
      console.error("Failed to fetch languages", e);
    }
  }, []);

  React.useEffect(() => {
    if (isActive) {
      fetch('/api/volume')
        .then((res) => res.json())
        .then((data) => {
          if (data.volume !== undefined && data.volume !== null) {
            setSystemVolume(data.volume);
          }
        })
        .catch((e) => console.error("Failed to fetch volume", e));
      refreshLanguageStatus();
    }
  }, [isActive, refreshLanguageStatus]);

  React.useEffect(() => {
    if (!isActive) return undefined;
    const timer = window.setInterval(refreshLanguageStatus, 300);
    return () => window.clearInterval(timer);
  }, [isActive, refreshLanguageStatus]);

  if (!isActive) return null

  const handleChange = (key, value) => {
    setConfig((prev) => ({ ...prev, [key]: value }))
  }

  const prepareLanguages = async (languages) => {
    try {
      const res = await fetch('/api/languages', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ languages }),
      });
      const data = await res.json();
      const nextStatus = {};
      for (const language of data.languages || []) {
        nextStatus[language.code] = language.status;
      }
      setLanguageStatus(nextStatus);
      window.setTimeout(refreshLanguageStatus, 1200);
    } catch (e) {
      console.error("Failed to prepare languages", e);
    }
  }

  const handleLanguageChange = (key, value) => {
    const next = { ...config, [key]: value };
    if (key === "lane1Language" && value === config.lane2Language) {
      next.lane2Language = config.lane1Language;
    }
    if (key === "lane2Language" && value === config.lane1Language) {
      next.lane1Language = config.lane2Language;
    }
    setConfig(next)
    prepareLanguages([next.lane1Language, next.lane2Language]);
  }

  const languageNameFor = (code) =>
    LANGUAGE_OPTIONS.find((language) => language.code === code)?.name || code;

  const handleVolumeChange = async (action) => {
    try {
      const res = await fetch('/api/volume', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action })
      });
      const data = await res.json();
      if (data.volume !== undefined && data.volume !== null) {
        setSystemVolume(data.volume);
      }
      playBlip("ping");
    } catch (e) {
      console.error('Failed to change volume:', e);
    }
  }

  const handleExitApp = async () => {
    try {
      await fetch('/api/kiosk/exit', { method: 'POST' });
    } catch (e) {
      console.error('Failed to exit app:', e);
    }
  }

  return (
    <div
      className={`settings-overlay ${isActive ? "active" : ""}`}
      style={{ display: isActive ? "flex" : "none" }}
    >
      <header className="overlay-header">
        <h2>Developer Settings</h2>
        <button className="overlay-close-btn" onClick={onClose}>
          ✕
        </button>
      </header>
      <div className="overlay-body">
        <div className="form-group" style={{ marginBottom: "10px" }}>
          <label>System Volume</label>
          <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
            <button
              className="overlay-btn"
              style={{ flex: 1 }}
              onClick={() => handleVolumeChange('down')}
            >
              -
            </button>
            <span style={{ minWidth: "40px", textAlign: "center", fontWeight: "bold" }}>
              {systemVolume !== null ? `${systemVolume}%` : "--"}
            </span>
            <button
              className="overlay-btn"
              style={{ flex: 1 }}
              onClick={() => handleVolumeChange('up')}
            >
              +
            </button>
          </div>
        </div>

        <div className="form-group">
          <label>Languages</label>
          <div className="language-select-row">
            <select
              value={config.lane1Language}
              onChange={(e) => handleLanguageChange("lane1Language", e.target.value)}
            >
              {LANGUAGE_OPTIONS.map((language) => (
                <option key={language.code} value={language.code}>
                  1 · {language.name}
                </option>
              ))}
            </select>
            <select
              value={config.lane2Language}
              onChange={(e) => handleLanguageChange("lane2Language", e.target.value)}
            >
              {LANGUAGE_OPTIONS.map((language) => (
                <option key={language.code} value={language.code}>
                  2 · {language.name}
                </option>
              ))}
            </select>
          </div>
          <div className="language-progress-list">
            {[config.lane1Language, config.lane2Language].map((code) => {
              const current = languageStatus[code] || {};
              const status = current.status || "not-installed";
              const progress = Number.isFinite(current.progress) ? current.progress : 0;
              const stage = current.stage || status;
              return (
                <div key={code} className="language-progress-item">
                  <div className="language-progress-meta">
                    <span>{code.toUpperCase()} · {languageNameFor(code)}</span>
                    <span>{status === "ready" ? "100%" : `${progress}%`}</span>
                  </div>
                  <div className={`language-progress-track ${status}`}>
                    <div
                      className="language-progress-fill"
                      style={{ width: `${Math.max(0, Math.min(100, progress))}%` }}
                    />
                  </div>
                  <div className="language-progress-stage">
                    {stage}
                  </div>
                </div>
              )
            })}
          </div>
        </div>

        <div className="form-group">
          <label>Theme Color</label>
          <div style={{ display: 'flex', gap: '10px', marginTop: '5px' }}>
            {THEME_COLORS.map((c) => (
              <button
                key={c.name}
                onClick={() => handleChange("themeColor", c.value)}
                title={c.name}
                style={{
                  width: '30px',
                  height: '30px',
                  borderRadius: '50%',
                  backgroundColor: c.value,
                  border: config.themeColor === c.value ? '2px solid #000' : '2px solid transparent',
                  boxShadow: config.themeColor === c.value ? '0 0 0 2px #fff' : 'none',
                  cursor: 'pointer',
                  padding: 0
                }}
              />
            ))}
          </div>
        </div>

        <div className="form-group">
          <label>API Endpoint</label>
          <div className="input-inline">
            <input
              type="text"
              value={config.endpointUrl}
              onChange={(e) => handleChange("endpointUrl", e.target.value)}
            />
            <button className="overlay-btn btn-sm" onClick={onTestConnection}>
              Test
            </button>
          </div>
        </div>

        <div className="form-group">
          <label>Model Name</label>
          <input
            type="text"
            value={config.modelName}
            onChange={(e) => handleChange("modelName", e.target.value)}
            list="model-suggestions"
          />
          <datalist id="model-suggestions">
            <option value="gemma4-e2b"></option>
            <option value="gemma-4-2b"></option>
          </datalist>
        </div>

        <div className="form-group">
          <label>API Key</label>
          <input
            type="password"
            placeholder="Optional api key"
            value={config.apiKey}
            onChange={(e) => handleChange("apiKey", e.target.value)}
          />
        </div>

        <div className="form-group">
          <label>Keyboard Mode</label>
          <select
            value={config.keyboardMode}
            onChange={(e) => handleChange("keyboardMode", e.target.value)}
          >
            <option value="landscape">
              Landscape — active person (Space / Z / ← →)
            </option>
            <option value="vertical">
              Vertical — two-hand (Z / X / ← → / − +)
            </option>
          </select>
        </div>

        <div className="form-row-checkboxes">
          <label className="checkbox-container">
            <input
              type="checkbox"
              checked={config.enableTts}
              onChange={(e) => handleChange("enableTts", e.target.checked)}
            />
            <span className="checkbox-label">Enable Speech Output</span>
          </label>
        </div>

        <div className="form-group">
          <label>Visualizer</label>
          <div className="slider-row">
            <div className="slider-group">
              <label>
                Bars: <span>{config.visualizerBars}</span>
              </label>
              <input
                type="range"
                min="8"
                max="128"
                step="8"
                value={config.visualizerBars}
                onChange={(e) => handleChange("visualizerBars", e.target.value)}
              />
            </div>
          </div>
        </div>

        <button className="overlay-btn" onClick={handleExitApp}>
          Exit App
        </button>
      </div>
    </div>
  )
}
