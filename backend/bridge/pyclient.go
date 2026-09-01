package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

// MLClient provides an interface to call the Python ML inference microservice.
type MLClient struct {
	baseURL    string
	httpClient *http.Client
}

func NewMLClient(baseURL string) *MLClient {
	return &MLClient{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: 60 * time.Second,
			Transport: &http.Transport{
				MaxIdleConns:        100,
				MaxIdleConnsPerHost: 20,
				IdleConnTimeout:     90 * time.Second,
			},
		},
	}
}

// CallSTT sends raw float32 samples to the Python STT recognizer.
func (c *MLClient) CallSTT(audio []float32, lang string) (string, error) {
	buf := new(bytes.Buffer)
	for _, s := range audio {
		if err := binary.Write(buf, binary.LittleEndian, s); err != nil {
			return "", err
		}
	}

	endpoint := fmt.Sprintf("%s/stt?lang=%s", c.baseURL, url.QueryEscape(lang))
	req, err := http.NewRequest("POST", endpoint, buf)
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/octet-stream")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("stt request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("stt error %d: %s", resp.StatusCode, string(body))
	}

	var res struct {
		Text  string `json:"text"`
		Error string `json:"error,omitempty"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&res); err != nil {
		return "", err
	}
	if res.Error != "" {
		return "", fmt.Errorf("stt service error: %s", res.Error)
	}
	return res.Text, nil
}

// CallTranslate invokes the translation model from src to dst language.
func (c *MLClient) CallTranslate(text, src, dst string) (string, error) {
	formData := url.Values{
		"text": {text},
		"src":  {src},
		"dst":  {dst},
	}

	endpoint := fmt.Sprintf("%s/translate", c.baseURL)
	resp, err := c.httpClient.PostForm(endpoint, formData)
	if err != nil {
		return "", fmt.Errorf("translate request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("translate error %d: %s", resp.StatusCode, string(body))
	}

	var res struct {
		Translation string `json:"translation"`
		Error       string `json:"error,omitempty"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&res); err != nil {
		return "", err
	}
	if res.Error != "" {
		return "", fmt.Errorf("translate service error: %s", res.Error)
	}
	return res.Translation, nil
}

// CallTTS synthesizes speech for the given text and returns raw 16kHz PCM16 bytes.
func (c *MLClient) CallTTS(text, lang string) ([]byte, error) {
	formData := url.Values{
		"text": {text},
		"lang": {lang},
	}

	endpoint := fmt.Sprintf("%s/tts", c.baseURL)
	resp, err := c.httpClient.PostForm(endpoint, formData)
	if err != nil {
		return nil, fmt.Errorf("tts request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("tts error %d: %s", resp.StatusCode, string(body))
	}

	audioBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("failed reading tts audio: %w", err)
	}
	return audioBytes, nil
}
