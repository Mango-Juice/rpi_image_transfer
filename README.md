# E-paper GPIO 통신 시스템

라즈베리파이 간 GPIO를 통한 신뢰성 있는 이미지 데이터 전송 시스템입니다.

## 📁 프로젝트 구조

```
시프/
├── drivers/           # 커널 드라이버
│   ├── tx_driver.c    # 송신 드라이버
│   ├── rx_driver.c    # 수신 드라이버
│   └── Makefile
├── apis/              # 사용자 라이브러리
│   ├── send_epaper_data.c/h      # 송신 API
│   ├── receive_epaper_data.c/h   # 수신 API
│   ├── stb_image.h               # 이미지 로딩 라이브러리
│   └── Makefile
├── programs/          # 응용 프로그램
│   ├── epaper_send.c     # 송신 프로그램
│   ├── epaper_receive.c  # 수신 프로그램
│   └── Makefile
└── Makefile          # 전체 빌드
```

## 🚀 빌드 방법

### 전체 빌드

```bash
cd 시프
make all
```

### 개별 컴포넌트 빌드

```bash
# 드라이버만 빌드
make drivers

# API 라이브러리만 빌드
make apis

# 응용 프로그램만 빌드
make programs
```

## 🔧 드라이버 설치

### 컴파일 (Ubuntu/Raspberry Pi OS)

```bash
cd drivers
make
```

### 모듈 로드

```bash
# TX 드라이버 로드
sudo insmod tx_driver.ko

# RX 드라이버 로드
sudo insmod rx_driver.ko

# 디바이스 노드 확인
ls -l /dev/epaper_*
```

### 디바이스 트리 설정

```dts
&gpio {
    epaper_tx: epaper-tx {
        compatible = "epaper,gpio-tx";
        data-gpios = <&gpio 2 GPIO_ACTIVE_HIGH>,
                     <&gpio 3 GPIO_ACTIVE_HIGH>,
                     <&gpio 4 GPIO_ACTIVE_HIGH>;
        clock-gpios = <&gpio 5 GPIO_ACTIVE_HIGH>;
        ack-gpios = <&gpio 6 GPIO_ACTIVE_LOW>;
    };

    epaper_rx: epaper-rx {
        compatible = "epaper,gpio-driver";
        rx-data-gpios = <&gpio 7 GPIO_ACTIVE_HIGH>,
                        <&gpio 8 GPIO_ACTIVE_HIGH>,
                        <&gpio 9 GPIO_ACTIVE_HIGH>;
        rx-clock-gpios = <&gpio 10 GPIO_ACTIVE_HIGH>;
        rx-ack-gpios = <&gpio 11 GPIO_ACTIVE_HIGH>;
    };
};
```

## 📡 통신 프로토콜

### 물리적 연결

- **데이터 라인**: 3개 (3-bit 병렬)
- **클럭 라인**: 1개 (동기화)
- **ACK/NACK 라인**: 1개 (응답)

### 프로토콜 특징

- **3-way 핸드셰이크**: 연결 설정
- **CRC32 검증**: 데이터 무결성
- **패킷 시퀀싱**: 순서 보장
- **자동 재전송**: 오류 복구

## 📚 API 사용법

### 송신 API

```c
#include "send_epaper_data.h"

// 기본 이미지 전송
int fd = epaper_open("/dev/epaper_tx");
epaper_send_image(fd, "image.jpg");
epaper_close(fd);

// 고급 옵션으로 전송
epaper_convert_options_t options = {
    .target_width = 400,
    .target_height = 300,
    .use_dithering = true,
    .threshold = 128,
    .invert_colors = false
};
epaper_send_image_advanced(fd, "image.png", &options);
```

### 수신 API

```c
#include "receive_epaper_data.h"

// 기본 이미지 수신
int fd = epaper_rx_open("/dev/epaper_rx");
epaper_image_t image;
if (epaper_receive_image(fd, &image)) {
    epaper_save_image_pbm(&image, "received.pbm");
    epaper_free_image(&image);
}
epaper_rx_close(fd);

// 고급 옵션으로 수신
epaper_receive_options_t options = {
    .verbose = true,
    .timeout_ms = 60000
};
epaper_receive_image_advanced(fd, &image, &options);
```

## 🖥️ 명령행 도구

### 이미지 송신 (`epaper_send`)

```bash
# 기본 전송
./epaper_send image.jpg

# 크기 조정하여 전송
./epaper_send -w 400 -h 300 image.png

# 디더링 적용
./epaper_send --dither --threshold 100 image.gif

# 색상 반전
./epaper_send --invert image.bmp

# 모든 옵션 적용
./epaper_send -d /dev/epaper_tx -w 800 -h 600 --dither --invert image.jpg
```

**옵션 설명:**

- `-d, --device <path>`: 디바이스 경로 (기본: /dev/epaper_tx)
- `-w, --width <pixels>`: 목표 너비
- `-h, --height <pixels>`: 목표 높이
- `-t, --threshold <0-255>`: 흑백 변환 임계값 (기본: 128)
- `--dither`: Floyd-Steinberg 디더링 적용
- `--invert`: 색상 반전
- `--help`: 도움말 표시

### 이미지 수신 (`epaper_receive`)

```bash
# 기본 수신 (PBM 형식)
./epaper_receive -o received.pbm

# RAW 바이너리 형식으로 수신
./epaper_receive -o image.raw -f raw

# 상세 출력 및 긴 타임아웃
./epaper_receive -v -t 60000 -o image.pbm

# 커스텀 디바이스
./epaper_receive -d /dev/epaper_rx -o output.pbm
```

**옵션 설명:**

- `-d, --device <path>`: RX 디바이스 경로 (기본: /dev/epaper_rx)
- `-o, --output <file>`: 출력 파일 경로
- `-f, --format <format>`: 출력 형식 (pbm, raw)
- `-t, --timeout <ms>`: 수신 타임아웃 (기본: 30000ms)
- `-v, --verbose`: 상세 출력
- `-h, --help`: 도움말 표시

## 🔄 전체 워크플로우 예시

### 송신 측 (라즈베리파이 A)

```bash
# 1. 드라이버 로드
sudo insmod tx_driver.ko

# 2. 이미지 전송
./epaper_send -w 400 -h 300 --dither photo.jpg
```

### 수신 측 (라즈베리파이 B)

```bash
# 1. 드라이버 로드
sudo insmod rx_driver.ko

# 2. 이미지 수신
./epaper_receive -v -o received_photo.pbm
```

## 🎯 지원 이미지 형식

### 입력 형식 (송신)

- **JPEG** (.jpg, .jpeg)
- **PNG** (.png)
- **BMP** (.bmp)
- **GIF** (.gif)
- **기타** stb_image 지원 형식

### 출력 형식 (수신)

- **PBM P4** (바이너리, 권장)
- **RAW** (차원 정보 + 바이너리 데이터)

## ⚡ 성능 및 제한사항

### 전송 속도

- **프로토콜**: 3-bit 병렬, 패킷 기반
- **처리량**: GPIO 속도에 따라 결정
- **신뢰성**: CRC32 + 재전송으로 100% 무결성 보장

### 제한사항

- **최대 이미지 크기**: 10000x10000 픽셀
- **메모리 사용량**: 이미지 크기에 비례
- **실시간성**: 핸드셰이크 및 재전송으로 인한 지연

## 🐛 문제 해결

### 일반적인 문제

1. **디바이스 열기 실패**: 권한 확인 (`sudo chmod 666 /dev/epaper_*`)
2. **GPIO 권한 오류**: 디바이스 트리 설정 확인
3. **전송 실패**: 케이블 연결 및 GPIO 핀 확인
4. **핸드셰이크 실패**: 양측 드라이버 로드 상태 확인

### 디버깅

```bash
# 커널 로그 확인
dmesg | grep epaper

# 드라이버 상태 확인
lsmod | grep epaper

# 디바이스 노드 확인
ls -l /dev/epaper_*
```

## 📄 라이선스

GPL 라이선스 하에 배포됩니다.
