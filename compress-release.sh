#!/bin/bash

# Directory path
dir_path=${1:-"./build"}

# Define the version
version=$(cat version)

# Function to map artifact directory name to platform and arch
map_artifact() {
  case "$1" in
    artifact-linux-20-04-release)
      echo "linux-x64"
      ;;
    artifact-mac-12-release)
      echo "darwin-x64"
      ;;
    artifact-mac-14-arm-release)
      echo "darwin-arm64"
      ;;
    artifact-windows-release)
      echo "windows-x64"
      ;;
    *)
      echo "unknown-unknown"
      ;;
  esac
}

# Iterate over each directory in the specified directory
for folder in "$dir_path"/artifact-*; do
  if [[ -d "$folder" ]]; then
    # Get the base name of the folder
    folder_name=$(basename "$folder")

    # Map the artifact directory name to platform and arch
    platform_arch=$(map_artifact "$folder_name")
    platform=$(echo "$platform_arch" | cut -d- -f1)
    arch=$(echo "$platform_arch" | cut -d- -f2)

    if [[ "$platform" == "unknown" || "$arch" == "unknown" ]]; then
      echo "Unknown directory pattern: $folder_name"
      continue
    fi

     # Create a new directory named with the version inside the artifact folder
      new_dir="${folder}/${version}"
      mkdir -p "$new_dir"

      # Move the contents of the artifact folder into the new version directory
      for item in "$folder"/*; do
        item_name=$(basename "$item")
        if [[ "$item_name" != "$version" ]]; then
          mv "$item" "$new_dir"
        fi
      done

    # Generate the new file name
    new_file_name="openocd-${platform}-${arch}-${version}.tar.gz"

    # Create the tar.gz archive
    tar -czf "${dir_path}/${new_file_name}" -C "$dir_path/${folder_name}" "$version"
    echo "Created archive: ${dir_path}/${new_file_name}"
  else
    echo "Skipping non-directory: $folder"
  fi
done
