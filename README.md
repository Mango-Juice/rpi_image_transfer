# E-paper GPIO 통신 시스템

라즈베리파이 간 GPIO를 통한 이미지 데이터 전송 시스템입니다.

## 📁 프로젝트 구조

```
시프/
├── drivers/           # 커널 드라이버
│   ├── tx_driver.c    # 송신 드라이버
│   ├── rx_driver.c    # 수신 드라이버
│   ├── epaper-gpio.dts # 디바이스 트리
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

## 🔧 핵심 특징

- **5-pin 시리얼 프로토콜**: Clock, Data, Start/Stop, ACK, NACK
- **CRC32 검증**: 데이터 무결성 보장
- **자동 재전송**: 오류 시 최대 3회 재시도
- **청크 기반 전송**: 대용량 이미지 지원
- **다양한 이미지 형식**: JPEG, PNG, BMP, GIF 지원

## 🚀 빌드 및 설치

### 전체 빌드

```bash
cd 시프
make all
```

### 드라이버 설치

```bash
cd drivers
make
sudo insmod tx_driver.ko  # 송신측
sudo insmod rx_driver.ko  # 수신측
```

### API 라이브러리

```bash
cd apis
make
# libepaper.so 및 libepaper.a 생성
```

## 📡 통신 프로토콜

### 물리적 연결 (5-pin)

| 신호       | 송신측 | 수신측 | 설명              |
|-----------|--------|--------|-------------------|
| CLOCK     | OUT    | IN     | 시리얼 동기화     |
| DATA      | OUT    | IN     | 1-bit 데이터      |
| START/STOP| OUT    | IN     | 전송 제어         |
| ACK       | IN     | OUT    | 수신 확인         |
| NACK      | IN     | OUT    | 수신 오류         |

### 프로토콜 특징

- **동기식 시리얼**: 1-bit 직렬 전송
- **블록 기반**: 헤더 + 데이터 + CRC32
- **오류 복구**: 자동 재전송 (최대 3회)
- **타임아웃**: 2초 응답 대기

## 📚 API 사용법

### 송신 API

```c
#include "send_epaper_data.h"

int fd = epaper_open("/dev/epaper_tx");
epaper_send_image(fd, "image.jpg");
epaper_close(fd);

// 고급 옵션
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

int fd = epaper_rx_open("/dev/epaper_rx");
epaper_image_t image;
if (epaper_receive_image(fd, &image)) {
    epaper_save_image_pbm(&image, "received.pbm");
    epaper_free_image(&image);
}
epaper_rx_close(fd);
```

## 🖥️ 명령행 도구

### 이미지 송신

```bash
./epaper_send image.jpg
./epaper_send -w 400 -h 300 --dither image.png
./epaper_send --invert --threshold 100 image.gif
```

### 이미지 수신

```bash
./epaper_receive -o received.pbm
./epaper_receive -v -t 60000 -o image.pbm
```

### 전체 워크플로우

**송신측 (라즈베리파이 A)**
```bash
sudo insmod tx_driver.ko
./epaper_send -w 400 -h 300 photo.jpg
```

**수신측 (라즈베리파이 B)**
```bash
sudo insmod rx_driver.ko
./epaper_receive -o received.pbm
```

## ⚡ 성능 및 특징

### 성능 지표

- **전송 속도**: ~100kHz 시리얼 클럭
- **최대 이미지**: 1920x1080 픽셀
- **신뢰성**: CRC32 + 재전송으로 100% 무결성
- **지원 형식**: JPEG, PNG, BMP, GIF → 1-bit PBM

### 시스템 요구사항

- **Linux 커널**: 4.19 이상
- **GPIO 핀**: 5개 (Clock, Data, Start/Stop, ACK, NACK)
- **메모리**: 이미지 크기에 비례

## 🐛 문제 해결

### 일반적인 문제

1. **디바이스 열기 실패**: `sudo chmod 666 /dev/epaper_*`
2. **전송 실패**: 케이블 연결 및 GPIO 핀 확인
3. **핸드셰이크 실패**: 양측 드라이버 로드 상태 확인

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
