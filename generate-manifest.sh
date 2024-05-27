#!/bin/bash

# Read the version from the version file
version=$(cat version)
# Directory containing the files
dir_path=${1:-"./build"}

# Function to generate manifest.json content
generate_manifest() {
  platform=$1
  arch=$2
  new_path=$3
  name=$4
  version=$5
  sha256=$6

echo "platform: $platform"
  cat <<EOL > ${dir_path}/${new_path}/${name}-${version}-manifest.json
{
  "name": "${name}",
  "version": "${version}",
  "main": "./bin",
  "url": "https://binaries.particle.io/openocd/${platform}/${arch}/${version}.tar.gz",
  "sha256": "${sha256}"
}
EOL
}

# Iterate over each file and generate the manifest.json
for file in "$dir_path"/*.tar.gz; do
  filename=$(basename -- "$file")
  # Extract platform and arch from the filename
  echo "doing something"
   platform=$(echo $filename | awk -F '-' '{print $2}')
    arch=$(echo $filename | awk -F '-' '{print $3}')

  if [[ -z $platform || -z $arch ]]; then
    echo "Unknown file pattern: $filename"
    continue
  fi

  # Define name
  name="openocd"

  # Generate the new file name without the extension
  new_file_name="${name}-${version}"
  new_path="${name}-${platform}-${arch}"
  # Create a directory for the current file to store the manifest.json
  echo "creating directory ${dir_path}/${new_path}"
  echo ${new_file_name}
  echo ${new_path}
  mkdir -p "${dir_path}/${new_path}"


  # Copy the original file to the new directory
  cp "$file" "${dir_path}/${new_path}/${new_file_name}.tar.gz"

  # Calculate the SHA-256 checksum
  sha256=$(sha256sum "$file" | awk '{ print $1 }')

  # Generate the manifest.json for the current file
  generate_manifest "$platform" "$arch" "$new_path" "$name" "$version" "$sha256"
  echo "Generated ${dir_path}/${new_path}/${new_file_name}/${name}-${version}-manifest.json"
done
