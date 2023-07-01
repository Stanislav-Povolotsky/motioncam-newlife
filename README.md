# About

Motion Cam is a camera application for Android that replaces the entire camera pipeline. It consumes RAW images and uses computational photography to combine multiple images to reduce noise. Additionally, it uses a single underexposed image to recover highlights and increase dynamic range.

You can install the latest version from [GitHub](https://github.com/Stanislav-Povolotsky/motioncam-newlife/releases/)
or a slightly out of date version from the [Play Store](https://play.google.com/store/apps/details?id=com.motioncam)

# RAW video capture

Capture RAW video up to 60 FPS on supported devices.

![Latest build](https://github.com/Stanislav-Povolotsky/motioncam-newlife/releases/download/8.0.8-beta2/MotionCam.8.0.8.apk)

### Convert to DNG on your PC

Download tools to convert the recorded videos to DNG on your PC:

![Windows](https://github.com/Stanislav-Povolotsky/motioncam-newlife/releases/download/8.1.0-beta1/motioncam-tools-0.22-win-x64.zip)

![Mac M1](https://github.com/Stanislav-Povolotsky/motioncam-newlife/releases/download/8.1.0-beta1/motioncam-tools-0.22-macos-m1.zip)

![Mac Intel](https://github.com/Stanislav-Povolotsky/motioncam-newlife/releases/download/8.0.8-beta2/motioncam-tools-0.11-macos-intel.dmg) (old version)

### Tutorials

https://youtu.be/-i_TvErm3C0

https://youtu.be/DX1FEtEnt5Y

## Other features

#### Dual exposure

Dual exposure is similar to the feature found in the Google Camera. The two sliders control the exposure compensation and tonemapping.

![Dual Exposure](https://user-images.githubusercontent.com/508688/118869074-d4f3de80-b8dc-11eb-8ca6-6261e3e1ea4d.gif)

#### Zero shutter lag burst capture

![Burst Capture](https://user-images.githubusercontent.com/508688/118869720-a7f3fb80-b8dd-11eb-8292-5e7a6ae899cc.gif)

#### Photo Mode

Photo mode captures RAW images in the background. It captures a single underexposed image when the shutter button is pressed which is used to recover highlights and increase dynamic range.

#### Night Mode

Night mode increases the shutter speed of the camera up to 1/3 of a second and captures more RAW images to further reduce noise.

## Overview

### Noise Reduction

The denoising algorithm uses bayer RAW images as input. Motion Cam treats the RAW data as four colour channels (red, blue and two green channels). It starts by creating an optical flow map between a set of images and the reference image utilising [Fast Optical Flow using Dense Inverse Search](https://arxiv.org/abs/1603.03590). Then, each colour channel is fused using a simplified Gaussian pyramid.

### Camera Preview

Motion Cam uses the GPU to generate a real time preview of the camera from its RAW data. It uses a simplified pipeline to produce an accurate representation of what the final image will look like. This means it is possible to adjust the tonemapping, contrast and colour settings in real time.

### Demosaicing

Most modern cameras use a bayer filter. This means the RAW image is subsampled and consists of 25% red, 25% blue and 50% green pixels. There are more green pixels because human vision is most sensitive to green light. The output from the denoising algorithm is demosaiced and colour corrected into an sRGB image. Motion Cam uses the algorithm [Color filter array demosaicking: New method and performance measures by Lu and Tan](https://pdfs.semanticscholar.org/37d2/87334f29698e451282f162cb4bc4f1f352d9.pdf).

### Tonemapping

Motion Cam uses the algorithm [exposure fusion](https://mericam.github.io/exposure_fusion/index.html) for tonemapping. The algorithm blends multiple different exposures to produce an HDR image. Instead of capturing multiple exposures, it artificially generates the overexposed image and uses the original exposure as inputs to the algorithm. The shadows slider in the app controls the overexposed image.

### Sharpening and Detail Enhancement

The details of the image are enhanced and sharpened using an unsharp mask with a threshold to avoid increasing the noise.

## Getting started

### MacOS

Install the following dependencies:

```
brew install cmake llvm python
```

Set the environment variables:

```
export ANDROID_NDK=[Path to Android NDK]
export LLVM_DIR=/usr/local/Cellar/llvm/[Installed LLVM version]
```

Run the ```./setupenv``` script to compile the dependencies needed by the project.

### Ubuntu

Install the following dependencies:

```
apt install git build-essential llvm-dev cmake clang libclang-dev
```

Set the environment variables:

```
export ANDROID_NDK=[Path to Android NDK]
export LLVM_DIR=/usr
```

Run the ```./setupenv``` script to compile the dependencies needed by the project.

## Running the application

After setting up the environment, open the project MotionCam-Android with Android Studio. It should compile and run.

### Generating code

MotionCam uses [Halide](https://github.com/halide/Halide) to generate the code for most of its algorithms. The generators can be found in ```libMotionCam/generators```. If you make any changes to the generator sources, use the script ```generate.sh``` to regenerate them.
