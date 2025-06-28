# E-paper GPIO 드라이버 가이드

GPIO를 통한 이미지 전송을 위한 리눅스 커널 드라이버입니다.

## 🔧 드라이버 구성

- **tx_driver.c**: 송신 드라이버 - 5-pin 시리얼 프로토콜
- **rx_driver.c**: 수신 드라이버 - 5-pin 시리얼 프로토콜

## 📡 프로토콜 사양

### GPIO 핀 구성

| 신호       | 송신측 | 수신측 | 설명          |
| ---------- | ------ | ------ | ------------- |
| CLOCK      | OUT    | IN     | 시리얼 동기화 |
| DATA       | OUT    | IN     | 1-bit 데이터  |
| START/STOP | OUT    | IN     | 전송 제어     |
| ACK        | IN     | OUT    | 수신 확인     |
| NACK       | IN     | OUT    | 수신 오류     |

### 통신 프로토콜

1. **시리얼 전송**: 1-bit 직렬 데이터 전송
2. **블록 단위**: 헤더 + 데이터 + CRC32 블록 전송
3. **ACK/NACK 응답**: 블록별 확인
4. **자동 재전송**: 오류 시 최대 3회 재시도

### 데이터 구조

```
Header (10 bytes): WIDTH(2) + HEIGHT(2) + DATA_LENGTH(4) + CHECKSUM(2)
Data (variable): 1-bit 압축된 이미지 데이터
CRC32 (4 bytes): 데이터 무결성 검증
```

## 🚀 설치 및 사용

### 1. 컴파일 및 로드

```bash
make clean
make
make install
ls -l /dev/epaper_*       # 디바이스 노드 확인
```

### 2. 디바이스 트리 설정

**TX 측 (송신)**

```dts
epaper_tx: epaper_tx_device {
    compatible = "epaper,gpio-tx";
    clock-gpios = <&gpio 13 0>;
    data-gpios = <&gpio 5 0>;
    start-stop-gpios = <&gpio 6 0>;
    ack-gpios = <&gpio 16 0>;
    nack-gpios = <&gpio 12 0>;
    status = "okay";
};
```

**RX 측 (수신)**

```dts
epaper_rx: epaper_rx_device {
    compatible = "epaper,gpio-rx";
    clock-gpios = <&gpio 21 0>;
    data-gpios = <&gpio 19 0>;
    start-stop-gpios = <&gpio 26 0>;
    ack-gpios = <&gpio 25 0>;
    nack-gpios = <&gpio 20 0>;
    status = "okay";
};
```

## 📝 파일 인터페이스

### TX 드라이버 (/dev/epaper_tx)

- **쓰기**: `write(fd, data, size)` - 자동 패킷화 및 전송

### RX 드라이버 (/dev/epaper_rx)

- **읽기**: `read(fd, buffer, size)` - 수신된 데이터 읽기
- **poll**: 데이터 대기 지원

## 🔍 상태 모니터링

```bash
# 실시간 로그
sudo dmesg -w | grep epaper

# 드라이버 상태
lsmod | grep epaper
```

## 🗂️ 모듈 제거

```bash
make uninstall
```
