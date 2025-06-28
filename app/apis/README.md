# E-paper API 라이브러리 사용 가이드

5-pin 시리얼 프로토콜을 사용하는 이미지 송수신을 위한 C 라이브러리입니다.

## 📦 라이브러리 구성

- **libepaper.a**: 정적 라이브러리
- **libepaper.so**: 동적 라이브러리
- **send_epaper_data.h**: 송신 API 헤더
- **receive_epaper_data.h**: 수신 API 헤더

## 🔧 설치

```bash
make clean
make
make install
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
