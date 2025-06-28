# 🖼️ E-paper 5-pin Serial Communication Project

![epaper](https://img.shields.io/badge/E--paper-GPIO%20Serial-blue)

라즈베리파이 등 임베디드 환경에서 5-pin GPIO 시리얼 프로토콜을 이용해 이미지를 E-paper로 송수신하는 오픈소스 프로젝트입니다.

## 📦 프로젝트 구성

- **device_driver/** : 5-pin 시리얼 프로토콜 기반 리눅스 커널 드라이버
- **app/apis/** : 이미지 송수신용 C API 라이브러리
- **app/programs/** : 명령행 송수신 응용 프로그램
- **Report.pdf** : 상세 설계/구현 보고서

## 🚀 빠른 시작

### 1. 드라이버 설치

```bash
cd device_driver
make clean
make
sudo make install
```

### 2. 라이브러리 설치

```bash
cd ../app/apis
make clean
make
sudo make install
```

### 3. 응용 프로그램 빌드 및 실행

```bash
cd ../programs
make
./epaper_send [options] <image_file>
./epaper_receive [options]
```

> 상세 옵션 및 예시는 각 디렉토리의 README.md 참고

## 🛠️ 주요 특징

- **5-pin 시리얼 프로토콜**: GPIO만으로 신뢰성 높은 데이터 전송 (Clock, Data, Start/Stop, ACK, NACK)
- **블록 단위 전송**: 헤더+데이터+CRC32, 블록별 ACK/NACK 및 자동 재전송
- **다양한 이미지 포맷 지원**: JPEG, PNG, BMP, GIF 등
- **사용자 친화적 API/CLI**: C 라이브러리 및 명령행 도구 제공
- **문서화**: 설치, 사용법, 드라이버/프로토콜/구조 설명, 문제 해결 가이드 포함

## 📚 문서

- 각 디렉토리별 README.md 참고
- 드라이버/프로토콜 상세: `device_driver/README.md`
- API 사용법: `app/apis/README.md`
- 프로그램 사용법: `app/programs/README.md`

## ⚠️ 주의사항

- 드라이버/라이브러리 설치 시 `sudo` 권한 필요
- 동적 라이브러리 사용 시 `LD_LIBRARY_PATH` 설정 필요할 수 있음
- 하드웨어 연결 전 핀 배치 및 방향 반드시 확인
