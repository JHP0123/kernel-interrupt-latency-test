# kernel-interrupt-latency-test

**버튼 인터럽트로 모스부호를 디코딩하는 리눅스 디바이스 드라이버를 통해 Threaded IRQ · Workqueue · Tasklet의 지연 시간을 ftrace로 비교 분석**

![C](https://img.shields.io/badge/C-00599C?logo=c&logoColor=white)
![Linux Kernel](https://img.shields.io/badge/Linux_Kernel-6.12-FCC624?logo=linux&logoColor=black)
![Raspberry Pi](https://img.shields.io/badge/Raspberry_Pi_4B-A22846?logo=raspberrypi&logoColor=white)
![License](https://img.shields.io/github/license/JHP0123/kernel-interrupt-latency-test)

> 같은 드라이버 로직에서 Bottom-Half 메커니즘만 바꿔 측정한 결과, 인터럽트 발생부터 BH 종료까지의 총 응답 시간은
> **Tasklet 약 30 µs < Threaded IRQ 약 55 µs < Workqueue 약 260 µs** 였습니다.
> 차이의 원인은 **실행 컨텍스트(Interrupt vs Process)** 와 **스케줄링 정책(RT vs 일반)** 에 있습니다.

<!-- TODO: 개발 기간, 개인 프로젝트 여부 -->

## 목차

1. [Overview & Hardware Setup](#1-overview--hardware-setup)
2. [System Architecture & Event Flow](#2-system-architecture--event-flow)
3. [Benchmark Results](#3-benchmark-results)
4. [In-Depth Latency Analysis](#4-in-depth-latency-analysis)
5. [Verification & Trace Logs](#5-verification--trace-logs)
6. [Getting Started](#6-getting-started)
7. [Limitations & Future Work](#7-limitations--future-work)

> 📘 배경 지식: [리눅스 커널 Bottom-Half 메커니즘 정리](./docs/bottom_half_theory.md)

---

## 1. Overview & Hardware Setup

| 항목 | 내용 |
|---|---|
| Board | Raspberry Pi 4B (BCM2711) |
| Kernel | Linux 6.12.75 |
| Trace Tool | ftrace `function_graph` |
| IRQ | 57 (GPIO 입력이 커널 내부에서 매핑된 인터럽트 번호) |

**배선**

| 신호 | Raspberry Pi 핀 |
|---|---|
| GND | 6번 또는 9번 핀 |
| VCC (3.3V) | 1번 핀 |
| GPIO Signal | GPIO 17 (물리 11번 핀) |

<!-- TODO: 실제 배선 확인. 기존 README의 "40번 핀"은 GPIO 21에 해당 -->

<img src="./docs/images/hardware_setup.png" width="400">

---

## 2. System Architecture & Event Flow

<img src="./docs/images/architecture_diagram.png" width="1000">

<!-- TODO: 다이어그램의 '250 ≤ ΔT' 분기를 pattern[i++] = '-' 로 수정 -->

| 단계 | 함수 | 컨텍스트 | 동작 |
|---|---|---|---|
| Top-Half | `button_th_handler` | HardIRQ | `gpiod_get_value()`로 핀 상태, `ktime_get()`으로 타임스탬프만 기록하고 BH 예약 |
| Bottom-Half | `button_bh_handler` | 기법별 상이 | 10ms 디바운싱, 250ms 기준 점/선 판별, `mod_timer()`로 700ms 타임아웃 갱신 |
| Timer Callback | `morse_timer_callback` | Timer (SoftIRQ) | 700ms 무입력 시 `decode_morse()` 호출 후 `dmesg` 출력 |

**비교한 BH 메커니즘**

| 기법 | BH 예약 방식 | BH 실행 주체 | 소스 |
|---|---|---|---|
| Tasklet | `tasklet_schedule()` | SoftIRQ (Interrupt Context) | [`tasklet_button_driver.c`](./src/tasklet_button_driver.c) |
| Threaded IRQ | `IRQ_WAKE_THREAD` 반환 | `irq/57-*` kthread, `SCHED_FIFO` | [`threaded_irq_button_driver_v2.c`](./src/threaded_irq_button_driver_v2.c) |
| Workqueue | `schedule_work()` | 공용 `kworker`, `SCHED_NORMAL` | [`workqueue_button_driver.c`](./src/workqueue_button_driver.c) |

---

## 3. Benchmark Results

### 요약 (단위: µs, 범위는 버튼 누름 / 뗌 각 1회)

| 기법 | TH 실행 | TH → BH 전환 | BH 실행 (누름 / 뗌) | **총 응답** |
|---|---|---|---|---|
| **Tasklet** | 11 ~ 16 | **6 ~ 7** | 6.6 / 15.2 | **29 ~ 32** |
| **Threaded IRQ** | 8 ~ 12 | 28 ~ 36 | 9.1 / 15.7 | **51 ~ 57** |
| **Workqueue** | **162 ~ 167** | 52 ~ 63 | 22.4 / 54.5 | **236 ~ 284** |

- TH 실행, TH→BH 전환은 ftrace 타임스탬프 차이, BH 실행은 ftrace DURATION 값 기준
- 총 응답 = TH 진입 ~ BH 종료

<details>
<summary>📋 원시 타임스탬프 표 (단위: 초, 마이크로초 해상도)</summary>

<img src="./docs/images/ftrace_comparison.png" width="1000">

</details>

---

## 4. In-Depth Latency Analysis

### 4.1 Top-Half 오버헤드: Workqueue만 10배 이상 느린 이유

- Tasklet과 Threaded IRQ의 TH는 각각 약 11 ~ 16 µs, 8 ~ 12 µs로 매우 짧음
- Workqueue는 TH 안에서 스케줄러 함수를 연쇄 호출하며 162 ~ 167 µs 소요

```text
button_th_handler
└─ queue_work_on
   └─ __queue_work
      └─ kick_pool → wake_up_process → try_to_wake_up
         └─ ttwu_do_activate
            ├─ enqueue_task_fair   ← 런큐에 kworker 삽입 (~47 µs)
            └─ wakeup_preempt → resched_curr   ← TIF_NEED_RESCHED 설정
```

- TH에서는 런큐 등록과 선점 플래그(`TIF_NEED_RESCHED`) 설정까지만 완료
- 실제 `schedule()`을 통한 kworker로의 Context Switch는 인터럽트 핸들러가 끝나고 복귀하는 시점에 발생

### 4.2 TH → BH 전환 지연: Interrupt vs Process Context

| 기법 | BH 컨텍스트 | 전환 지연 | 이유 |
|---|---|---|---|
| Tasklet | Interrupt (SoftIRQ) | 6 ~ 7 µs | HardIRQ 종료 직후 컨텍스트 전환 없이 곧바로 BH 실행 |
| Threaded IRQ | Process (`irq/57-*`) | 28 ~ 36 µs | 스케줄러가 `<idle>`에서 전용 kthread로 Context Switch |
| Workqueue | Process (`kworker`) | 52 ~ 63 µs | 스케줄러가 `<idle>`에서 공용 kworker로 Context Switch |

### 4.3 Threaded IRQ vs Workqueue: 스케줄링 정책의 차이

- 둘 다 Process Context지만 총 응답 시간은 **Threaded IRQ(51 ~ 57 µs)가 Workqueue(236 ~ 284 µs)보다 약 4 ~ 5배 빠름**
- Threaded IRQ: 전용 인터럽트 스레드를 실시간 스케줄링 클래스(`SCHED_FIFO`)로 관리
- Workqueue: 공용 워커 풀(pwq) 관리를 거치고 일반 태스크와 같은 fair 클래스(`SCHED_NORMAL`, 6.6+ EEVDF) 경로를 타서 추가 연산 발생

### 4.4 버튼 뗌(Release)에서 BH가 더 오래 걸리는 이유

| 기법 | 누름 (0) | 뗌 (1) |
|---|---|---|
| Threaded IRQ | 9.05 µs | 15.72 µs |
| Workqueue | 22.35 µs | 54.48 µs |
| Tasklet | 6.55 µs | 15.24 µs |

- **누름**: 기존 타이머를 취소하는 `timer_delete()`만 호출 (약 0.6 ~ 2.1 µs)
- **뗌**: 700ms 타임아웃을 재등록하는 `mod_timer()`가 타이머 서브시스템 오버헤드 유발 (약 8.5 ~ 32.3 µs)

---

## 5. Verification & Trace Logs

### 동작 검증 (`dmesg`)

버튼으로 입력한 모스부호가 인터럽트를 거쳐 알파벳으로 디코딩된 결과입니다.

| Threaded IRQ | Workqueue | Tasklet |
|---|---|---|
| <img src="./docs/images/morse-test-dmesg.png"> | <img src="./docs/images/workqueue-dmesg.png"> | <img src="./docs/images/tasklet-dmesg.png"> |
| 그림 1. Threaded IRQ | 그림 2. Workqueue | 그림 3. Tasklet |

### Raw ftrace Logs

| 기법 | 로그 |
|---|---|
| Threaded IRQ | [`threaded_irq_ftrace.log`](./docs/logs/threaded_irq_ftrace.log) |
| Workqueue | [`workqueue.log`](./docs/logs/workqueue.log) |
| Tasklet | [`tasklet.log`](./docs/logs/tasklet.log) |

---

## 6. Getting Started

### Requirements

- Raspberry Pi 4B, Linux 6.12 커널 소스 (빌드된 트리)
- 크로스 컴파일 시 `aarch64-linux-gnu-` 툴체인
- 버튼을 GPIO 17에 연결하는 Device Tree overlay (`compatible = "button"`)

<!-- TODO: 사용한 DT overlay 파일을 저장소에 추가하고 적용 방법 기재 -->

### Build

`Makefile`에서 빌드할 드라이버의 `obj-m` 줄 주석을 해제한 뒤 빌드합니다.

```bash
# Raspberry Pi에서 직접 빌드
make KDIR=/path/to/linux

# 크로스 컴파일
make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

### Load & Test

```bash
sudo insmod tasklet_button_driver.ko
dmesg -w          # 버튼으로 모스부호 입력 → 디코딩 결과 확인
sudo rmmod tasklet_button_driver
```

### ftrace 측정

```bash
cd /sys/kernel/tracing
echo function_graph > current_tracer
echo button_th_handler button_bh_handler > set_graph_function
echo 1 > options/funcgraph-abstime
echo 1 > tracing_on
# 버튼 입력
echo 0 > tracing_on
cat trace
```

<!-- TODO: 실제 사용한 ftrace 옵션과 일치하는지 확인 -->

---

## 7. Limitations & Future Work

**Limitations**
- 기법별 측정이 버튼 누름·뗌 각 1회로, 통계적 대표성이 부족함
- `function_graph`는 함수마다 계측 비용이 붙어, 호출 체인이 깊은 Workqueue TH는 실제보다 적게 측정되었을 수 있음
- 무부하 상태에서만 측정함

---

## License

[LICENSE](LICENSE) 참조
