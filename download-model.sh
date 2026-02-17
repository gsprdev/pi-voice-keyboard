#!/bin/bash
# Download Whisper models in GGML format

set -e

MODEL_DIR="speech-models"
BASE_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

# All paths relative to this script
cd "$(dirname "${BASH_SOURCE[0]}")"

# Create models directory
mkdir -p "$MODEL_DIR"

# Available models with sizes
declare -A MODELS=(
    ["tiny.en"]="ggml-tiny.en.bin (75 MB)"
    ["tiny"]="ggml-tiny.bin (75 MB)"
    ["base.en"]="ggml-base.en.bin (142 MB)"
    ["base"]="ggml-base.bin (142 MB)"
    ["small.en"]="ggml-small.en.bin (466 MB)"
    ["small"]="ggml-small.bin (466 MB)"
    ["medium.en"]="ggml-medium.en.bin (1.5 GB)"
    ["medium"]="ggml-medium.bin (1.5 GB)"
    ["large-v1"]="ggml-large-v1.bin (2.9 GB)"
    ["large-v2"]="ggml-large-v2.bin (2.9 GB)"
    ["large-v3"]="ggml-large-v3.bin (2.9 GB)"
)

show_help() {
    echo "Usage: $0 <model-name>"
    echo ""
    echo "Download Whisper models in GGML format for use with the transcription service."
    echo ""
    echo "Available models:"
    for model in "${!MODELS[@]}"; do
        printf "  %-12s - %s\n" "$model" "${MODELS[$model]}"
    done
    echo ""
    echo "Examples:"
    echo "  $0 medium.en    # English-only medium model (recommended)"
    echo "  $0 base.en      # Faster, less accurate"
    echo "  $0 large-v3     # Most accurate, slowest"
    echo ""
    echo "Note: .en models are English-only and faster/better for English."
}

if [ $# -eq 0 ]; then
    show_help
    exit 0
fi

MODEL_NAME="$1"

# Validate model name
if [ -z "${MODELS[$MODEL_NAME]}" ]; then
    echo "Error: Unknown model '$MODEL_NAME'"
    echo ""
    show_help
    exit 1
fi

# Determine filename
if [[ "$MODEL_NAME" == *.en ]]; then
    FILENAME="ggml-${MODEL_NAME}.bin"
    OUTPUT_NAME="${MODEL_DIR}/en_whisper_${MODEL_NAME%.en}.ggml"
else
    FILENAME="ggml-${MODEL_NAME}.bin"
    OUTPUT_NAME="${MODEL_DIR}/whisper_${MODEL_NAME}.ggml"
fi

URL="${BASE_URL}/${FILENAME}"

echo "Downloading Whisper model: $MODEL_NAME"
echo "From: $URL"
echo "To: $OUTPUT_NAME"
echo ""

# Check if file already exists
if [ -f "$OUTPUT_NAME" ]; then
    echo "Model already exists: $OUTPUT_NAME"
    read -p "Overwrite? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Download cancelled."
        exit 0
    fi
fi

# Download the model
echo "Downloading... (this may take a while)"
if command -v wget &> /dev/null; then
    wget -O "$OUTPUT_NAME" "$URL"
elif command -v curl &> /dev/null; then
    curl -L -o "$OUTPUT_NAME" "$URL"
else
    echo "Error: Neither wget nor curl found. Please install one of them."
    exit 1
fi

echo ""
echo "Download complete: $OUTPUT_NAME"
echo ""
echo "File size:"
ls -lh "$OUTPUT_NAME" | awk '{print "  " $5}'
echo ""
echo "To use this model, update the modelPath in transcribe-whisper/main.go or set MODEL_PATH environment variable."
