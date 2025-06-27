# E-paper GPIO 드라이버 가이드

GPIO를 통한 이미지 전송을 위한 리눅스 커널 드라이버입니다.

## 🔧 드라이버 구성

- **tx_driver.c**: 송신 드라이버 (TX)
- **rx_driver.c**: 수신 드라이버 (RX)

## 📡 프로토콜 사양

### GPIO 핀 구성

| 신호    | 송신측 | 수신측 | 설명          |
| ------- | ------ | ------ | ------------- |
| DATA[0] | OUT    | IN     | 데이터 비트 0 |
| DATA[1] | OUT    | IN     | 데이터 비트 1 |
| DATA[2] | OUT    | IN     | 데이터 비트 2 |
| CLOCK   | OUT    | IN     | 동기화 클럭   |
| ACK     | IN     | OUT    | 응답 신호     |

### 통신 프로토콜

1. **3-way 핸드셰이크**: 연결 설정
2. **패킷 기반 전송**: 헤더 + 데이터 + CRC32
3. **ACK/NACK 응답**: 패킷별 확인
4. **자동 재전송**: 오류 시 최대 5회 재시도

### 패킷 구조

```
┌─────────┬──────────┬─────────┬─────────┐
│ SEQ_NUM │ DATA_LEN │  DATA   │  CRC32  │
│ (1byte) │ (1byte)  │(0-31)   │(4bytes) │
└─────────┴──────────┴─────────┴─────────┘
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
    epaper_tx: epaper-tx {
        compatible = "epaper,gpio-tx";
        data-gpios = <&gpio 2 GPIO_ACTIVE_HIGH>,    // DATA[0]
                     <&gpio 3 GPIO_ACTIVE_HIGH>,    // DATA[1]
                     <&gpio 4 GPIO_ACTIVE_HIGH>;    // DATA[2]
        clock-gpios = <&gpio 5 GPIO_ACTIVE_HIGH>;   // CLOCK
        ack-gpios = <&gpio 6 GPIO_ACTIVE_LOW>;      // ACK (input)
        status = "okay";
    };
};
```

### RX 측 (수신) 설정

```dts
&gpio {
    epaper_rx: epaper-rx {
        compatible = "epaper,gpio-driver";
        rx-data-gpios = <&gpio 7 GPIO_ACTIVE_HIGH>,    // DATA[0]
                        <&gpio 8 GPIO_ACTIVE_HIGH>,    // DATA[1]
                        <&gpio 9 GPIO_ACTIVE_HIGH>;    // DATA[2]
        rx-clock-gpios = <&gpio 10 GPIO_ACTIVE_HIGH>;  // CLOCK
        rx-ack-gpios = <&gpio 11 GPIO_ACTIVE_HIGH>;    // ACK (output)
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
[epaper_tx] TX driver initialized successfully
[epaper_tx] Handshake successful
[epaper_tx] Packet 0 sent successfully after 1 attempts
[epaper_rx] RX driver initialized successfully
[epaper_rx] Handshake SYN received, sending ACK
[epaper_rx] Packet 0 received successfully (31 bytes, CRC32 OK)
```

## ⚡ 성능 특성

### 전송 속도

- **클럭 속도**: GPIO 설정에 따라 결정
- **패킷 크기**: 최대 31바이트 데이터
- **오버헤드**: 헤더(2) + CRC32(4) = 6바이트

### 신뢰성

- **CRC32 검증**: 100% 데이터 무결성
- **재전송 메커니즘**: 오류 시 자동 복구
- **타임아웃 처리**: 데드락 방지

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

- **최대 패킷 크기**: 31바이트
- **GPIO 핀 수**: 5개 (데이터 3 + 클럭 1 + ACK 1)
- **동시 사용**: 각 디바이스당 하나의 프로세스만
- **플랫폼**: 라즈베리파이 및 호환 SBC

## 🗂️ 모듈 제거

```bash
# 드라이버 언로드
sudo rmmod tx_driver
sudo rmmod rx_driver

# 디바이스 노드 자동 제거됨
```

## 📄 라이선스

GPL v2 라이선스 하에 배포됩니다.
