#pragma once
#include <driver/i2s.h>
#include <SPIFFS.h>

// ================================
// I2S CONFIG FOR INMP441
// ================================
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// YOU WILL SET THESE FROM THE INO:
extern int I2S_PIN_WS;
extern int I2S_PIN_SCK;
extern int I2S_PIN_SD;

// Recording parameters
#define SAMPLE_RATE     16000
#define WAV_BITS        16
#define MIC_BUFFER_SIZE 1024   // per read, small for steady streaming

File wavFile;
bool micIsRecording = false;
int totalPcmBytes = 0;

// ================================
// WAV HEADER CREATION + FIX
// ================================
void writeWavHeader(File &f) {
    uint8_t header[44];

    // RIFF chunk descriptor
    memcpy(header, "RIFF", 4);
    uint32_t chunkSize = 0;          // will update later
    memcpy(header + 4, &chunkSize, 4);
    memcpy(header + 8, "WAVE", 4);

    // fmt sub-chunk
    memcpy(header + 12, "fmt ", 4);
    uint32_t subChunk1Size = 16;
    memcpy(header + 16, &subChunk1Size, 4);
    uint16_t audioFormat = 1;        // PCM
    memcpy(header + 20, &audioFormat, 2);
    uint16_t numChannels = 1;
    memcpy(header + 22, &numChannels, 2);
    uint32_t sampleRate = SAMPLE_RATE;
    memcpy(header + 24, &sampleRate, 4);
    uint32_t byteRate = SAMPLE_RATE * (WAV_BITS / 8) * numChannels;
    memcpy(header + 28, &byteRate, 4);
    uint16_t blockAlign = numChannels * (WAV_BITS / 8);
    memcpy(header + 32, &blockAlign, 2);
    uint16_t bitsPerSample = WAV_BITS;
    memcpy(header + 34, &bitsPerSample, 2);

    // data sub-chunk
    memcpy(header + 36, "data", 4);
    uint32_t dataSize = 0;           // update later
    memcpy(header + 40, &dataSize, 4);

    f.write(header, 44);
}

void fixWavHeader(File &f, int pcmBytes) {
    f.seek(4);
    uint32_t chunkSize = pcmBytes + 36;
    f.write((uint8_t*)&chunkSize, 4);

    f.seek(40);
    f.write((uint8_t*)&pcmBytes, 4);
}


// ================================
// INIT MICROPHONE
// ================================
void initMicrophone() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_PIN_SCK,
        .ws_io_num  = I2S_PIN_WS,
        .data_out_num = -1,
        .data_in_num  = I2S_PIN_SD
    };

    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);

    Serial.println("MIC: INIT OK");
}


// ================================
// START RECORDING
// ================================
void startMicRecording() {
    Serial.println("MIC: START");

    wavFile = SPIFFS.open("/record.wav", "w");
    writeWavHeader(wavFile);

    totalPcmBytes = 0;
    micIsRecording = true;
}


// ================================
// READ MIC & APPEND TO WAV
// ================================
void recordMicChunk() {
    if (!micIsRecording) return;

    int32_t rawBuf[MIC_BUFFER_SIZE];
    size_t bytesRead = 0;

    i2s_read(I2S_PORT, (void*)rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);

    // Convert 32-bit samples → 16-bit PCM for WAV
    int samples = bytesRead / 4;
    for (int i = 0; i < samples; i++) {
        int32_t v = rawBuf[i] >> 14;      // scale down
        int16_t pcm = (int16_t)v;
        wavFile.write((uint8_t*)&pcm, 2);
        totalPcmBytes += 2;
    }
}


// ================================
// STOP RECORDING
// ================================
void stopMicRecording() {
    if (!micIsRecording) return;

    micIsRecording = false;
    fixWavHeader(wavFile, totalPcmBytes);
    wavFile.close();

    Serial.println("MIC: STOP + SAVED /record.wav");
}