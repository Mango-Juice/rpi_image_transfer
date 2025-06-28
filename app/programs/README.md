# E-paper 프로그램 사용 가이드

이 디렉토리는 5-pin 시리얼 프로토콜 기반 E-paper 이미지 송수신용 응용 프로그램을 포함합니다.

## 📦 프로그램 구성

- **epaper_send**: 이미지 파일을 송신 디바이스로 전송
- **epaper_receive**: 수신 디바이스에서 이미지를 수신 및 저장

## 🔧 빌드 및 설치

```bash
make clean
make
```

## 🖼️ 사용법

### 1. 이미지 송신 (epaper_send)

```bash
./epaper_send [options] <image_file>
```

#### 주요 옵션

- `-d, --device <path>`: 송신 디바이스 경로 (기본값: /dev/epaper_tx)
- `-w, --width <pixels>`: 타겟 너비
- `-h, --height <pixels>`: 타겟 높이
- `-t, --threshold <0-255>`: 임계값 (기본: 128)
- `-D, --dither`: Floyd-Steinberg 디더링 적용
- `-i, --invert`: 색상 반전
- `--help`: 도움말 출력

#### 예시

```bash
./epaper_send -d /dev/epaper_tx -w 800 -h 600 -D -i sample.png
```

### 2. 이미지 수신 (epaper_receive)

```bash
./epaper_receive [options]
```

#### 주요 옵션

- `-d, --device <path>`: 수신 디바이스 경로 (기본값: /dev/epaper_rx)
- `-o, --output <file>`: 저장 파일 경로
- `-f, --format <format>`: 저장 형식(raw, pbm)
- `-t, --timeout <ms>`: 수신 타임아웃 (기본: 30000)
- `-v, --verbose`: 상세 출력
- `-h, --help`: 도움말 출력

#### 예시

```bash
./epaper_receive -o received.pbm
./epaper_receive -d /dev/epaper_rx -o image.raw -f raw -v
```

## ⚠️ 참고 사항

- 수신 후 반드시 `epaper_free_image()`로 메모리 해제 필요
- 지원 입력: JPEG, PNG, BMP, GIF 등
- 출력: PBM(P4), RAW
- 통신 오류, 타임아웃 등은 표준 에러코드(ETIMEDOUT, ECOMM, EBUSY)로 반환
- **일부 환경에서는 sudo 권한이 필요할 수 있음**

## 🧹 정리

```bash
make clean
```
