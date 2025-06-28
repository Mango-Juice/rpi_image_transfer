# E-paper 프로젝트 전체 실행 가이드

이 문서는 E-paper 5-pin 시리얼 통신 기반 전체 프로젝트의 설치 및 실행 순서를 안내합니다.

## 1️⃣ 라이브러리 설치

```bash
cd app/apis
make clean
make
make install
```

## 2️⃣ 응용 프로그램 빌드 및 실행

```bash
cd ../programs
make
./epaper_send [options] <image_file>
./epaper_receive [options]
```

- 상세 옵션 및 예시는 각 디렉토리의 README.md를 참고하세요.

## 참고

- 드라이버 설치 및 디바이스 트리 설정은 device_driver/README.md를 참고하세요.
- sudo 권한이 필요할 수 있습니다.
