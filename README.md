# kernel-interrupt-latency-test
A Linux device driver that prints Morse code via button interrupts, benchmarking the execution time of Threaded IRQ, Workqueue, and Tasklet.

## 1. Overview & Hardware Setup
- Raspberry Pi 4B
- Linux Kernel Version: 6.12.75 (6.12.XX)
- Trace Tool: ftrace function_graph
- Hardware Setup
  - GND: 6th pin or 9th pin
  - VCC / 3.3V: 1th pin
  - GPIO Signal: Raspberry Pi 4B GPIO 17 (40th pin)

<img src="./docs/images/hardware_setup.png" width="400">

- IRQ 57
  - BCM2835 SoC의 GPIO 핀 입력이 커널 내부에서 매핑된 인터럽트 번호로, `request_threaded_irq`를 통해 Top-Half 핸들러가 바인딩되어 버튼 입력을 비동기적으로 감지

## 2. System Architecture & Event Flow

<img src="./docs/images/architecture_diagram.png" width="1200">

- Top-Half (button_th_handler)
  - 하드웨어 인터럽트 컨텍스트에서 gpiod_get_value()로 핀 상태 확인 및 ktime_get() 타임스탬프만 즉각 기록
 
- Bottom-Half (button_bh_handler)
  - 10ms 디바운싱(노이즈 필터링), 250ms 기준 점(.)과 선(-) 판별 및 mod_timer()를 통한 700ms 타임아웃 갱신
 
- Timer Callback (morse_timer_callback)
  - 700ms 무입력 시 decode_morse() 호출 및 dmesg 출력

## 3. Quantitative Benchmark Results

<img src="./docs/images/ftrace_comparison.png" width="1200">
- TH Entry/Exit & BH Entry/Exit Unit of Time
  - [seconds].[milliseconds]

  ### 3-1. In-Depth Latency Analysis



  ### 3-2. Verification & Trace Logs
