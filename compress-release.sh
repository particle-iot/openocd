#!/bin/bash

# Directory path
dir_path=${1:-"./build"}

# Iterate over each folder in the specified directory
for folder in "$dir_path"/openocd-*; do
  if [[ -d $folder ]]; then
    # Create the tar.gz archive
    tar -czf "${folder}.tar.gz" -C "$dir_path" "$(basename "$folder")"
    echo "Created archive: ${folder}.tar.gz"
  else
    echo "Skipping non-directory: $folder"
  fi
done
