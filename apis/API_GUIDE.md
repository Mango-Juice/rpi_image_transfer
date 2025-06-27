# E-paper API 라이브러리 사용 가이드

5-pin 시리얼 프로토콜을 사용하는 이미지 송수신을 위한 C 라이브러리입니다.

## 📦 라이브러리 구성

- **libepaper.a**: 정적 라이브러리
- **libepaper.so**: 동적 라이브러리
- **send_epaper_data.h**: 송신 API 헤더
- **receive_epaper_data.h**: 수신 API 헤더

## 🔧 설치

```bash
cd apis
make
sudo make install
```

## 📡 송신 API (5-pin 시리얼 프로토콜)

### 헤더 포함

```c
#include <send_epaper_data.h>
```

### 기본 함수들

#### `epaper_open()`

```c
int epaper_open(const char* device_path);
```

송신 디바이스를 엽니다.

- **매개변수**: 디바이스 경로 (예: "/dev/epaper_tx")
- **반환값**: 파일 디스크립터 (실패 시 -1)

#### `epaper_close()`

```c
void epaper_close(int fd);
```

디바이스를 닫습니다.

#### `epaper_send_image()`

```c
bool epaper_send_image(int fd, const char* image_path);
```

기본 설정으로 이미지를 전송합니다.

- **매개변수**:
  - `fd`: 디바이스 파일 디스크립터
  - `image_path`: 이미지 파일 경로
- **반환값**: 성공 시 true

#### `epaper_send_image_resized()`

```c
bool epaper_send_image_resized(int fd, const char *image_path,
                              int target_width, int target_height);
```

크기를 조정하여 이미지를 전송합니다.

#### `epaper_send_image_advanced()`

```c
bool epaper_send_image_advanced(int fd, const char *image_path,
                               const epaper_convert_options_t *options);
```

고급 옵션으로 이미지를 전송합니다.

### 옵션 구조체

```c
typedef struct {
    int target_width;      // 목표 너비 (0 = 원본 유지)
    int target_height;     // 목표 높이 (0 = 원본 유지)
    bool use_dithering;    // 디더링 적용 여부
    bool invert_colors;    // 색상 반전 여부
    int threshold;         // 흑백 변환 임계값 (0-255)
} epaper_convert_options_t;
```

### 사용 예시

```c
#include <send_epaper_data.h>

int main() {
    // 디바이스 열기
    int fd = epaper_open("/dev/epaper_tx");
    if (fd < 0) {
        return 1;
    }

    // 기본 전송
    if (!epaper_send_image(fd, "photo.jpg")) {
        printf("전송 실패\n");
    }

    // 고급 옵션 전송
    epaper_convert_options_t options = {
        .target_width = 400,
        .target_height = 300,
        .use_dithering = true,
        .invert_colors = false,
        .threshold = 128
    };

    epaper_send_image_advanced(fd, "image.png", &options);

    epaper_close(fd);
    return 0;
}
```

## 📥 수신 API

### 헤더 포함

```c
#include <receive_epaper_data.h>
```

### 기본 함수들

#### `epaper_rx_open()`

```c
int epaper_rx_open(const char* device_path);
```

수신 디바이스를 엽니다.

#### `epaper_rx_close()`

```c
void epaper_rx_close(int fd);
```

디바이스를 닫습니다.

#### `epaper_receive_image()`

```c
bool epaper_receive_image(int fd, epaper_image_t *image);
```

기본 설정으로 이미지를 수신합니다.

#### `epaper_receive_image_advanced()`

```c
bool epaper_receive_image_advanced(int fd, epaper_image_t *image,
                                  const epaper_receive_options_t *options);
```

고급 옵션으로 이미지를 수신합니다.

#### `epaper_save_image_pbm()`

```c
bool epaper_save_image_pbm(const epaper_image_t *image, const char *filename);
```

이미지를 PBM P4 바이너리 형식으로 저장합니다.

#### `epaper_save_image_raw()`

```c
bool epaper_save_image_raw(const epaper_image_t *image, const char *filename);
```

이미지를 RAW 바이너리 형식으로 저장합니다.

#### `epaper_free_image()`

```c
void epaper_free_image(epaper_image_t *image);
```

이미지 메모리를 해제합니다.

### 구조체들

```c
// 이미지 데이터
typedef struct {
    uint32_t width;           // 이미지 너비
    uint32_t height;          // 이미지 높이
    unsigned char *data;      // 1-bit 바이너리 데이터
    size_t data_size;         // 데이터 크기 (바이트)
} epaper_image_t;

// 수신 옵션
typedef struct {
    bool save_raw;            // RAW 형식 자동 저장
    const char *output_path;  // 자동 저장 경로
    bool verbose;             // 상세 출력
    int timeout_ms;           // 타임아웃 (밀리초)
} epaper_receive_options_t;
```

### 사용 예시

```c
#include <receive_epaper_data.h>

int main() {
    // 디바이스 열기
    int fd = epaper_rx_open("/dev/epaper_rx");
    if (fd < 0) {
        return 1;
    }

    epaper_image_t image;

    // 기본 수신
    if (epaper_receive_image(fd, &image)) {
        printf("수신 완료: %ux%u\n", image.width, image.height);

        // PBM 형식으로 저장
        epaper_save_image_pbm(&image, "received.pbm");

        // 메모리 해제
        epaper_free_image(&image);
    }

    // 고급 옵션 수신
    epaper_receive_options_t options = {
        .verbose = true,
        .timeout_ms = 60000
    };

    if (epaper_receive_image_advanced(fd, &image, &options)) {
        epaper_save_image_raw(&image, "received.raw");
        epaper_free_image(&image);
    }

    epaper_rx_close(fd);
    return 0;
}
```

## 🔗 컴파일 방법

### 정적 라이브러리 사용

```bash
gcc -I./apis myprogram.c -L./apis -lepaper -lm -o myprogram
```

### 동적 라이브러리 사용

```bash
gcc -I./apis myprogram.c -L./apis -lepaper -lm -o myprogram
export LD_LIBRARY_PATH=./apis:$LD_LIBRARY_PATH
./myprogram
```

### Makefile 예시

```makefile
CC = gcc
CFLAGS = -Wall -O2 -I./apis
LIBS = -L./apis -lepaper -lm

myprogram: myprogram.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
```

## ⚠️ 중요 사항

### 메모리 관리

- `epaper_receive_image()` 후 반드시 `epaper_free_image()` 호출
- 실패 시에도 부분적으로 할당된 메모리는 자동으로 해제됨

### 오류 처리

라이브러리는 다양한 종류의 오류를 구분하여 상세한 정보를 제공합니다:

#### 드라이버 레벨 오류 코드

- **ETIMEDOUT**: 수신측 응답 타임아웃
- **ECOMM**: 통신 오류 (NACK 수신)
- **EHOSTUNREACH**: 수신측에 도달할 수 없음 (핸드셰이크 타임아웃)
- **ECONNREFUSED**: 수신측에서 연결 거부 (핸드셰이크 NACK)
- **ECONNRESET**: 연결이 재설정됨
- **EIO**: 일반적인 입출력 오류

#### 오류 메시지 예시

```c
// 타임아웃 발생 시
Error: Connection timeout at byte 1024/4096

// NACK 수신 시
Error: Communication error (NACK) at byte 2048/4096

// 수신측 미응답 시
Error: Receiver not reachable while sending width
```

#### 오류 대응 방법

1. **ETIMEDOUT**: 네트워크 상태 확인, 수신측 동작 확인
2. **ECOMM**: 데이터 무결성 문제, 재전송 시도
3. **EHOSTUNREACH**: 수신측 전원/연결 상태 확인
4. **ECONNREFUSED**: 수신측 프로그램 상태 확인

### 스레드 안전성

- 라이브러리는 스레드 안전하지 않음
- 멀티스레드 환경에서는 외부 동기화 필요

### 커널 타이밍 개선

드라이버는 정밀한 타이밍을 위해 다음과 같이 최적화되었습니다:

- **usleep_range()**: msleep() 대신 더 정확한 마이크로초 단위 지연
- **버스트 보호**: 클록 신호 폭주 감지 및 자동 복구
- **상태머신 방어**: 비정상 입력에 대한 자동 초기화
- **오류 세분화**: 실패 원인별 구체적인 errno 반환

## 🎯 지원 형식

### 입력 이미지 (송신)

stb_image 라이브러리가 지원하는 모든 형식:

- JPEG, PNG, BMP, TGA, PSD, GIF, HDR, PIC, PNM

### 출력 형식 (수신)

- **PBM P4**: 표준 바이너리 비트맵 (권장)
- **RAW**: 사용자 정의 바이너리 형식
