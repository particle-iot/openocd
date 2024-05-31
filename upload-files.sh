 #!/bin/bash

 # Set the base S3 bucket path
  S3_BUCKET_PATH=${1}/openocd

  # Function to upload files to S3
  upload_to_s3() {
     local platform=$1
     local arch=$2
     local dir_path="build/release/openocd-${platform}-${arch}"

     if [ -d "$dir_path" ]; then
         echo "aws s3 cp "$dir_path" "${S3_BUCKET_PATH}/${platform}/${arch}" --recursive"
     else
         echo "Directory $dir_path does not exist"
     fi
  }

  # List of platforms and architectures to upload
  for dir in build/release/*; do
     if [[ -d "$dir" ]]; then
         dir_name=$(basename "$dir")
         IFS='-' read -r _ platform arch <<< "$dir_name"
         upload_to_s3 "$platform" "$arch"
     fi
  done
