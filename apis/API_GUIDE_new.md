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

## 📡 송신 API

### 기본 함수들

```c
#include <send_epaper_data.h>

int epaper_open(const char* device_path);
void epaper_close(int fd);
bool epaper_send_image(int fd, const char* image_path);
bool epaper_send_image_advanced(int fd, const char *image_path,
                               const epaper_convert_options_t *options);
```

### 옵션 구조체

```c
typedef struct {
    int target_width, target_height;  // 크기 조정
    bool use_dithering;               // 디더링 적용
    bool invert_colors;               // 색상 반전
    int threshold;                    // 흑백 변환 임계값
} epaper_convert_options_t;
```

### 사용 예시

```c
#include <send_epaper_data.h>

int main() {
    int fd = epaper_open("/dev/epaper_tx");
    if (fd < 0) return 1;

    // 기본 전송
    epaper_send_image(fd, "image.jpg");

    // 고급 옵션 전송
    epaper_convert_options_t options = {
        .target_width = 400,
        .target_height = 300,
        .use_dithering = true,
        .threshold = 128,
        .invert_colors = false
    };
    epaper_send_image_advanced(fd, "image.png", &options);

    epaper_close(fd);
    return 0;
}
```

## 📨 수신 API

### 기본 함수들

```c
#include <receive_epaper_data.h>

int epaper_rx_open(const char* device_path);
void epaper_rx_close(int fd);
bool epaper_receive_image(int fd, epaper_image_t *image);
bool epaper_save_image_pbm(const epaper_image_t *image, const char *filename);
void epaper_free_image(epaper_image_t *image);
```

### 구조체들

```c
typedef struct {
    uint32_t width, height;
    unsigned char *data;      // 1-bit 데이터
    size_t data_size;
} epaper_image_t;
```

### 사용 예시

```c
#include <receive_epaper_data.h>

int main() {
    int fd = epaper_rx_open("/dev/epaper_rx");
    if (fd < 0) return 1;

    epaper_image_t image;
    if (epaper_receive_image(fd, &image)) {
        printf("수신 완료: %ux%u\n", image.width, image.height);
        epaper_save_image_pbm(&image, "received.pbm");
        epaper_free_image(&image);
    }

    epaper_rx_close(fd);
    return 0;
}
```

## 🔧 빌드 및 링크

```bash
# 정적 라이브러리
gcc -I./apis myprogram.c -L./apis -lepaper -lm -o myprogram

# 동적 라이브러리
gcc -I./apis myprogram.c -L./apis -lepaper -lm -o myprogram
export LD_LIBRARY_PATH=./apis:$LD_LIBRARY_PATH
```

## ⚠️ 중요 사항

### 메모리 관리

- `epaper_receive_image()` 후 반드시 `epaper_free_image()` 호출

### 오류 처리

- **ETIMEDOUT**: 수신측 응답 타임아웃
- **ECOMM**: 통신 오류 (NACK 수신)
- **EBUSY**: 디바이스 사용 중

### 지원 형식

- **입력**: JPEG, PNG, BMP, GIF 등
- **출력**: PBM P4 바이너리, RAW 형식
