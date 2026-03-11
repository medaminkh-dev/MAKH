#!/bin/bash

# Script to gather all .asm, .c, .h files and linker.ld into one formatted txt file

# Get timestamp
TIMESTAMP=$(date +%s)

# Count files
IDCOUNT=$(find . -type f \( -name "*.asm" -o -name "*.c" -o -name "*.h" -o -name "linker.ld" \) | wc -l)

# Output filename
OUTPUT_FILE="progress/total_${IDCOUNT}_${TIMESTAMP}.txt"

# Clear output file
> "$OUTPUT_FILE"

# Function to process files
process_files() {
    local ext=$1
    local files=$(find . -type f -name "$ext" | sort)
    
    for file in $files; do
        echo "NAME :$file" >> "$OUTPUT_FILE"
        echo "DIR:" >> "$OUTPUT_FILE"
        echo "(CODE)" >> "$OUTPUT_FILE"
        cat "$file" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
        echo "----()" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
    done
}

# Process linker.ld first
if [ -f "linker.ld" ]; then
    echo "NAME :linker.ld" >> "$OUTPUT_FILE"
    echo "DIR:" >> "$OUTPUT_FILE"
    echo "(CODE)" >> "$OUTPUT_FILE"
    cat "linker.ld" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
    echo "----()" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
fi

# Process .asm files
process_files "*.asm"

# Process .c files
process_files "*.c"

# Process .h files
process_files "*.h"

echo "Created: $OUTPUT_FILE"
echo "Total files: $IDCOUNT"
echo "Timestamp: $TIMESTAMP"
