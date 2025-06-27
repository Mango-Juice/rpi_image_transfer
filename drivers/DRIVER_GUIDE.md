# E-paper GPIO 드라이버 가이드

GPIO를 통한 이미지 전송을 위한 리눅스 커널 드라이버입니다.

## 🔧 드라이버 구성

- **tx_driver.c**: 송신 드라이버 (TX) - 5-pin 시리얼 프로토콜
- **rx_driver.c**: 수신 드라이버 (RX) - 5-pin 시리얼 프로토콜

## 📡 프로토콜 사양 (5-pin 시리얼)

### GPIO 핀 구성

| 신호       | 송신측 | 수신측 | 설명                  |
| ---------- | ------ | ------ | --------------------- |
| CLOCK      | OUT    | IN     | 시리얼 클럭 (동기화)  |
| DATA       | OUT    | IN     | 시리얼 데이터 (1-bit) |
| START/STOP | OUT    | IN     | 전송 시작/종료 신호   |
| ACK        | IN     | OUT    | 수신 확인 응답        |
| NACK       | IN     | OUT    | 수신 오류 응답        |

### 통신 프로토콜

1. **시리얼 전송**: 1-bit 직렬 데이터 전송
2. **블록 단위**: 헤더 + 데이터 + CRC32 블록 전송
3. **ACK/NACK 응답**: 블록별 확인
4. **자동 재전송**: 오류 시 최대 3회 재시도

### 데이터 구조

```
┌──────────┬───────────┬─────────────┬──────────┐
│  HEADER  │   DATA    │   CRC32     │          │
│ (10bytes)│ (variable)│  (4bytes)   │          │
└──────────┴───────────┴─────────────┴──────────┘

Header 구조:
┌───────┬────────┬─────────────┬─────────────────┐
│ WIDTH │ HEIGHT │ DATA_LENGTH │ HEADER_CHECKSUM │
│(2byte)│(2byte) │  (4bytes)   │    (2bytes)     │
└───────┴────────┴─────────────┴─────────────────┘
```

## 🚀 설치 및 사용

### 1. 컴파일

```bash
cd drivers
make
```

### 2. 모듈 로드

```bash
# TX 드라이버 로드
sudo insmod tx_driver.ko

# RX 드라이버 로드
sudo insmod rx_driver.ko
```

### 3. 디바이스 노드 확인

```bash
ls -l /dev/epaper_*
# /dev/epaper_tx - 송신 디바이스
# /dev/epaper_rx - 수신 디바이스
```

### 4. 권한 설정 (필요시)

```bash
sudo chmod 666 /dev/epaper_tx
sudo chmod 666 /dev/epaper_rx
```

## 🔗 디바이스 트리 설정

### TX 측 (송신) 설정

```dts
&gpio {
    epaper_tx: epaper_tx_device {
        compatible = "epaper,gpio-tx";
        clock-gpios = <&gpio 13 0>;        // Clock output
        data-gpios = <&gpio 5 0>;          // Data output
        start-stop-gpios = <&gpio 6 0>;    // Start/Stop output
        ack-gpios = <&gpio 16 0>;          // ACK input
        nack-gpios = <&gpio 12 0>;         // NACK input
        status = "okay";
    };
};
```

### RX 측 (수신) 설정

```dts
&gpio {
    epaper_rx: epaper_rx_device {
        compatible = "epaper,gpio-rx";
        clock-gpios = <&gpio 21 0>;        // Clock input
        data-gpios = <&gpio 19 0>;         // Data input
        start-stop-gpios = <&gpio 26 0>;   // Start/Stop input
        ack-gpios = <&gpio 25 0>;          // ACK output
        nack-gpios = <&gpio 20 0>;         // NACK output
        status = "okay";
    };
};
```

## 📝 파일 인터페이스

### TX 드라이버 (/dev/epaper_tx)

- **열기**: `open("/dev/epaper_tx", O_WRONLY)`
- **쓰기**: `write(fd, data, size)` - 자동 패킷화 및 전송
- **닫기**: `close(fd)`

### RX 드라이버 (/dev/epaper_rx)

- **열기**: `open("/dev/epaper_rx", O_RDONLY)`
- **읽기**: `read(fd, buffer, size)` - 수신된 데이터 읽기
- **닫기**: `close(fd)`
- **poll**: 데이터 대기 지원

## 🔍 상태 모니터링

### 커널 로그 확인

```bash
# 실시간 로그 보기
sudo dmesg -w | grep epaper

# 최근 로그만 보기
dmesg | grep epaper | tail -20
```

### 로그 메시지 예시

```
[epaper_tx] Driver loaded successfully
[epaper_tx] Image sent successfully
[epaper_tx] NACK received, retrying
[epaper_rx] Driver loaded successfully
[epaper_rx] Image received successfully (1920x1080, 2073600 bytes)
[epaper_rx] CRC32 validation passed
```

## ⚡ 성능 특성

### 전송 속도

- **시리얼 클럭**: 약 100kHz (udelay 타이밍)
- **이미지 크기**: 최대 1920x1080 픽셀
- **오버헤드**: 헤더(10) + CRC32(4) = 14바이트

### 신뢰성

- **CRC32 검증**: 100% 데이터 무결성
- **재전송 메커니즘**: 오류 시 자동 복구 (최대 3회)
- **타임아웃 처리**: 2초 타임아웃으로 데드락 방지

## 🐛 문제 해결

### 일반적인 문제들

#### 1. 모듈 로드 실패

```bash
# 문제: insmod 실패
# 해결: 커널 헤더 설치 확인
sudo apt-get install linux-headers-$(uname -r)
```

#### 2. 디바이스 노드가 생성되지 않음

```bash
# 문제: /dev/epaper_* 없음
# 해결: 디바이스 트리 설정 확인 및 재부팅
sudo reboot
```

#### 3. GPIO 권한 오류

```bash
# 문제: GPIO 접근 실패
# 해결: 사용자를 gpio 그룹에 추가
sudo usermod -a -G gpio $USER
```

#### 4. 핸드셰이크 실패

```bash
# 원인: 케이블 연결 문제 또는 GPIO 핀 충돌
# 해결:
# 1. 케이블 연결 확인
# 2. GPIO 핀 사용 현황 확인
# 3. 디바이스 트리 GPIO 번호 확인
```

### 디버깅 명령어

```bash
# 모듈 상태 확인
lsmod | grep epaper

# GPIO 사용 현황
cat /sys/kernel/debug/gpio

# 디바이스 트리 확인
cat /proc/device-tree/gpio/epaper-*/compatible

# IRQ 사용 현황
cat /proc/interrupts | grep epaper
```

## 🔧 고급 설정

### 커널 모듈 매개변수

현재 버전은 매개변수를 지원하지 않습니다. 모든 설정은 디바이스 트리를 통해 이루어집니다.

### 성능 튜닝

1. **클럭 속도 조정**: 디바이스 트리에서 GPIO 드라이브 강도 설정
2. **IRQ 우선순위**: 실시간 커널 사용 시 IRQ 우선순위 조정
3. **버퍼 크기**: 소스 코드에서 FIFO_SIZE 조정 후 재컴파일

## 📊 제한사항

- **최대 이미지 크기**: 1920x1080 픽셀
- **GPIO 핀 수**: 5개 (Clock + Data + Start/Stop + ACK + NACK)
- **동시 사용**: 각 디바이스당 하나의 프로세스만
- **플랫폼**: 라즈베리파이 및 호환 SBC
- **데이터 형식**: 바이너리 이미지 데이터 (width + height + data)

## 🗂️ 모듈 제거

```bash
# 드라이버 언로드
sudo rmmod tx_driver
sudo rmmod rx_driver

# 디바이스 노드 자동 제거됨
```

## 📄 라이선스

GPL v2 라이선스 하에 배포됩니다.
