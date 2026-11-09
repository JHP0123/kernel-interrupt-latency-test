# kernel-interrupt-latency-test
A Linux device driver that prints Morse code via button interrupts, benchmarking the execution time of Threaded IRQ, Workqueue, and Tasklet.

[An In-depth Analysis of the Linux Kernel's Bottom-Half Interrupt Processing Mechanism](./docs/bottom_half_theory.md)

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
  - 하드웨어 인터럽트 컨텍스트에서 `gpiod_get_value()`로 핀 상태 확인 및 `ktime_get()` 타임스탬프만 즉각 기록
 
- Bottom-Half (button_bh_handler)
  - 10ms 디바운싱(노이즈 필터링), 250ms 기준 점(.)과 선(-) 판별 및 `mod_timer()`를 통한 700ms 타임아웃 갱신
 
- Timer Callback (morse_timer_callback)
  - 700ms 무입력 시 `decode_morse()` 호출 및 `dmesg` 출력

## 3. Quantitative Benchmark Results

<img src="./docs/images/ftrace_comparison.png" width="1200">
- TH Entry/Exit & BH Entry/Exit Unit of Time
  - [seconds].[milliseconds]

### 3-1. In-Depth Latency Analysis

- **Top-Half 내 메커니즘에 따른 오버헤드 격차**
  - Tasklet 및 Threaded IRQ
    - Tasklet과 Threaded IRQ는 TH 실행 완료까지 각각 약 11 ~ 16 µs, 8 ~ 12 µs의 매우 짧은 시간만을 소모.
    - Workqueue의 스케줄링 개입 오버헤드. Workqueue는 TH(`button_th_handler`) 내부에서 `queue_work_on()` -> `try_to_wake_up()` -> `enqueue_task_fair()` 순으로 커널 스케줄러 함수를 연쇄 호출. 이 과정에서 CFS(Completely Fair Scheduler) 런큐에 워커 스레드를 삽입. TH 구간에서만 162 ~ 167 µs의 높은 오버헤드가 발생. TH 내부에서 CFS 런큐 등록과 함께 `resched_curr()`를 통한 선점 플래그인 `TIF_NEED_RESCHED` 설정이 완료되며, 실제 스케줄링(`schedule()`)을 통한 워커 스레드로의 Context Switch은 인터럽트 핸들러가 종료되고 복귀하는 시점에 발생
   
- **Interrupt vs Process Context 차이와 TH에서 BH 전환 지연**
    - Tasklet은 하드웨어 IRQ가 끝난 직후 커널이 인터럽트 컨텍스트(SoftIRQ)를 유지한 채 곧바로 BH를 실행하므로, TH 종료부터 BH 진입까지의 지연 시간이 6 ~ 7 µs로 기록.
    - Threaded IRQ와 Workqueue는 BH가 별도의 커널 스레드(irq/57-... 또는 kworker)라는 Process Context에서 동작. 따라서 TH 종료 후 스케줄러가 개입해 <idle> 상태에서 해당 커널 스레드로 Context Switch을 수행해야 하므로 각각 28 ~ 36 µs(Threaded IRQ), 52 ~ 63µs(Workqueue)의 스케줄링 지연이 발생.
 
- **스케줄링 정책(RT vs CFS)에 따른 Threaded IRQ와 Workqueue의 성능 차이**
    - 같은 Process Context 기반임에도 Threaded IRQ(총 51 ~ 57 µs)가 Workqueue(총 236 ~ 284 µs)보다 약 4.5~5배 빠른 총 응답 시간을 기록
    - Threaded IRQ가 전용 인터럽트 스레드를 실시간 스케줄링 클래스(`SCHED_FIFO`, RT 우선순위)로 관리하지만, 일반 Workqueue는 공용 워커 풀인 pwq 관리를 거치고 일반 태스크와 동일한 CFS(`SCHED_NORMAL`) 스케줄링 경로를 타기 때문에 CFS에 필요한 연산이 추가가 되는 오버헤드 존재.
 
- **버튼 누름(Press) 대비 버튼 뗌(Release) 시의 BH 실행 시간 증가**
  - 세 가지 기법 모두 버튼 누름(0)보다 버튼 뗌(1) 이벤트에서 BH 실행 시간(BH time)이 뚜렷하게 증가
    - Threaded IRQ: 9.05 µs -> 15.72 µs, Workqueue: 22.35 µs -> 54.48 µs, Tasklet: 6.55 µs -> 15.24 µs
  - ftrace 로그에서 확인되듯, 누름(0) 시점에는 기존 타이머를 취소하는 `timer_delete()`가 약 0.6 ~ 2.1 µs만에 끝나지만, 뗌(1) 시점에는 700ms 타임아웃을 재등록하기 위해 호출한 `mod_timer()`가 추가적인 커널 타이머 서브시스템 오버헤드(약 8.5 ~ 32.3 µs)를 유발하기 때문

### 3-2. Verification & Trace Logs

- **동작 검증 (`dmesg`)**
    - 버튼을 눌러 입력된 모스부호가 인터럽트를 통해 성공적으로 알파벳으로 디코딩되어 출력되는 커널 로그 결과
<p align="center">
  <img src="./docs/images/morse-test-dmesg.png" alt="ThreadedIRQ dmesg morse output log" width="700">
</p>
<p align="center">
  <em>▲ 그림 1. ThreadedIRQ dmesg를 통한 모스부호 디코딩 출력 결과</em>
</p>

<p align="center">
  <img src="./docs/images/workqueue-dmesg.png" alt="Workqueue dmesg morse output log" width="700">
</p>
<p align="center">
  <em>▲ 그림 1. Workqueue dmesg를 통한 모스부호 디코딩 출력 결과</em>
</p>

<p align="center">
  <img src="./docs/images/tasklet-dmesg.png" alt="Tasklet dmesg morse output log" width="700">
</p>
<p align="center">
  <em>▲ 그림 1. Tasklet dmesg를 통한 모스부호 디코딩 출력 결과</em>
</p>

-  log file

<details><summary>View ThreadedIRQ Raw ftrace Log</summary>

```tracer: function_graph

TIME       CPU  DURATION                  FUNCTION CALLS

|          |     |   |                     |   |   |   |

3641.138185 |   0)               |  button_th_handler threaded_irq_button_driver_v2 {
3641.138188 |   0)               |    gpiod_get_value() {
3641.138189 |   0)               |      gpiod_get_raw_value_commit() {
3641.138190 |   0)               |        bcm2835_gpio_get() {
3641.138191 |   0)   1.000 us    |          gpiochip_get_data();
3641.138193 |   0)   2.759 us    |        } /* bcm2835_gpio_get /
3641.138194 |   0)   4.593 us    |      } / gpiod_get_raw_value_commit /
3641.138194 |   0)   6.426 us    |    } / gpiod_get_value /
3641.138195 |   0)               |    ktime_get() {
3641.138196 |   0)   0.575 us    |      arch_counter_read();
3641.138197 |   0)   1.870 us    |    } / ktime_get /
3641.138197 |   0) + 14.556 us   |  } / button_th_handler [threaded_irq_button_driver_v2] /
3641.138233 |   2)               |  button_bh_handler threaded_irq_button_driver_v2 {
3641.138235 |   2)   1.019 us    |    _raw_spin_lock_irqsave();
3641.138237 |   2)   0.685 us    |    _raw_spin_unlock_irqrestore();
3641.138238 |   2)   0.740 us    |    timer_delete();
3641.138239 |   2)   0.611 us    |    _raw_spin_lock_irqsave();
3641.138240 |   2)   0.685 us    |    _raw_spin_unlock_irqrestore();
3641.138242 |   2)   9.056 us    |  } / button_bh_handler [threaded_irq_button_driver_v2] /
3641.281142 |   0)               |  button_th_handler threaded_irq_button_driver_v2 {
3641.281143 |   0)               |    gpiod_get_value() {
3641.281144 |   0)               |      gpiod_get_raw_value_commit() {
3641.281144 |   0)               |        bcm2835_gpio_get() {
3641.281145 |   0)   0.574 us    |          gpiochip_get_data();
3641.281146 |   0)   1.981 us    |        } / bcm2835_gpio_get /
3641.281147 |   0)   3.204 us    |      } / gpiod_get_raw_value_commit /
3641.281147 |   0)   4.445 us    |    } / gpiod_get_value /
3641.281148 |   0)               |    ktime_get() {
3641.281149 |   0)   0.592 us    |      arch_counter_read();
3641.281150 |   0)   1.944 us    |    } / ktime_get /
3641.281150 |   0)   8.963 us    |  } / button_th_handler [threaded_irq_button_driver_v2] /
3641.281178 |   2)               |  button_bh_handler threaded_irq_button_driver_v2 {
3641.281179 |   2)   0.648 us    |    _raw_spin_lock_irqsave();
3641.281180 |   2)   0.686 us    |    _raw_spin_unlock_irqrestore();
3641.281182 |   2)   0.741 us    |    _raw_spin_lock_irqsave();
3641.281183 |   2)   0.574 us    |    _raw_spin_unlock_irqrestore();
3641.281184 |   2)               |    mod_timer() {
3641.281185 |   2)               |      lock_timer_base() {
3641.281186 |   2)   0.815 us    |        _raw_spin_lock_irqsave();
3641.281187 |   2)   2.241 us    |      } / lock_timer_base /
3641.281188 |   2)   0.648 us    |      detach_if_pending();
3641.281189 |   2)   0.685 us    |      calc_wheel_index();
3641.281190 |   2)   0.722 us    |      enqueue_timer();
3641.281192 |   2)   0.685 us    |      _raw_spin_unlock_irqrestore();
3641.281193 |   2)   8.574 us    |    } / mod_timer /
3641.281193 |   2) + 15.723 us   |  } / button_bh_handler [threaded_irq_button_driver_v2] */

```
</details>


<details><summary>View Workqueue Raw ftrace Log</summary>

```tracer: function_graph

TIME       CPU  DURATION                  FUNCTION CALLS

|          |     |   |                     |   |   |   |

9996.970111 |   0)               |  button_th_handler workqueue_button_driver {
9996.970118 |   0)               |    gpiod_get_value() {
9996.970121 |   0)               |      gpiod_get_raw_value_commit() {
9996.970123 |   0)               |        bcm2835_gpio_get() {
9996.970125 |   0)   2.315 us    |          gpiochip_get_data();
9996.970130 |   0)   6.814 us    |        } /* bcm2835_gpio_get /
9996.970132 |   0) + 11.389 us   |      } / gpiod_get_raw_value_commit /
9996.970133 |   0) + 15.741 us   |    } / gpiod_get_value /
9996.970135 |   0)               |    ktime_get() {
9996.970137 |   0)   1.741 us    |      arch_counter_read();
9996.970140 |   0)   5.315 us    |    } / ktime_get /
9996.970142 |   0)               |    queue_work_on() {
9996.970144 |   0)   1.797 us    |      clear_pending_if_disabled();
9996.970148 |   0)               |      __queue_work() {
9996.970150 |   0)   1.648 us    |        __rcu_read_lock();
9996.970153 |   0)   1.814 us    |        _raw_spin_lock();
9996.970157 |   0)   1.871 us    |        pwq_tryinc_nr_active();
9996.970160 |   0)   1.648 us    |        get_pwq();
9996.970163 |   0)               |        kick_pool() {
9996.970165 |   0)               |          wake_up_process() {
9996.970167 |   0)               |            try_to_wake_up() {
9996.970169 |   0)   2.037 us    |              _raw_spin_lock_irqsave();
9996.970173 |   0)   1.963 us    |              kthread_is_per_cpu();
9996.970176 |   0)   1.760 us    |              ttwu_queue_wakelist();
9996.970180 |   0)   1.685 us    |              _raw_spin_lock();
9996.970183 |   0)   1.907 us    |              update_rq_clock();
9996.970187 |   0)               |              ttwu_do_activate() {
9996.970188 |   0)               |                enqueue_task() {
9996.970190 |   0)               |                  enqueue_task_fair() {
9996.970192 |   0)               |                    enqueue_entity() {
9996.970194 |   0)   1.870 us    |                      update_curr();
9996.970198 |   0)   1.945 us    |                      __update_load_avg_se();
9996.970201 |   0)   1.833 us    |                      __update_load_avg_cfs_rq();
9996.970205 |   0)               |                      dbs_update_util_handler() {
9996.970206 |   0)   1.871 us    |                        cpufreq_this_cpu_can_update();
9996.970210 |   0)   5.129 us    |                      } / dbs_update_util_handler /
9996.970211 |   0)   1.685 us    |                      update_cfs_group();
9996.970215 |   0)               |                      place_entity() {
9996.970216 |   0)   1.630 us    |                        avg_vruntime();
9996.970220 |   0)   5.130 us    |                      } / place_entity /
9996.970222 |   0)   1.907 us    |                      __enqueue_entity();
9996.970225 |   0) + 32.907 us   |                    } / enqueue_entity /
9996.970227 |   0)   1.871 us    |                    dl_server_update_idle_time();
9996.970230 |   0)   1.871 us    |                    dl_server_start();
9996.970234 |   0)   1.648 us    |                    hrtick_update();
9996.970237 |   0) + 46.945 us   |                  } / enqueue_task_fair /
9996.970239 |   0) + 50.315 us   |                } / enqueue_task /
9996.970240 |   0)               |                wakeup_preempt() {
9996.970242 |   0)   1.852 us    |                  resched_curr();
9996.970246 |   0)   5.259 us    |                } / wakeup_preempt /
9996.970249 |   0) + 62.518 us   |              } / ttwu_do_activate /
9996.970251 |   0)   1.759 us    |              _raw_spin_unlock();
9996.970254 |   0)   1.740 us    |              _raw_spin_unlock_irqrestore();
9996.970258 |   0) + 90.759 us   |            } / try_to_wake_up /
9996.970259 |   0) + 94.111 us   |          } / wake_up_process /
9996.970261 |   0) + 97.667 us   |        } / kick_pool /
9996.970263 |   0)   1.722 us    |        _raw_spin_unlock();
9996.970266 |   0)   1.704 us    |        __rcu_read_unlock();
9996.970269 |   0) ! 121.500 us  |      } / __queue_work /
9996.970271 |   0) ! 128.592 us  |    } / queue_work_on /
9996.970273 |   0) ! 163.704 us  |  } / button_th_handler [workqueue_button_driver] */

-0    =>   kworker-2578

9996.970325 |   0)               |  button_bh_handler workqueue_button_driver {
9996.970327 |   0)   1.871 us    |    _raw_spin_lock_irqsave();
9996.970331 |   0)   1.871 us    |    _raw_spin_unlock_irqrestore();
9996.970334 |   0)   1.926 us    |    timer_delete();
9996.970338 |   0)   1.852 us    |    _raw_spin_lock_irqsave();
9996.970341 |   0)   1.796 us    |    _raw_spin_unlock_irqrestore();
9996.970347 |   0) + 22.352 us   |  } /* button_bh_handler [workqueue_button_driver] */

kworker-2578  =>    -0

9997.092452 |   0)               |  button_th_handler workqueue_button_driver {
9997.092459 |   0)               |    gpiod_get_value() {
9997.092461 |   0)               |      gpiod_get_raw_value_commit() {
9997.092464 |   0)               |        bcm2835_gpio_get() {
9997.092466 |   0)   2.037 us    |          gpiochip_get_data();
9997.092470 |   0)   6.481 us    |        } /* bcm2835_gpio_get /
9997.092472 |   0) + 10.796 us   |      } / gpiod_get_raw_value_commit /
9997.092473 |   0) + 15.056 us   |    } / gpiod_get_value /
9997.092475 |   0)               |    ktime_get() {
9997.092477 |   0)   1.852 us    |      arch_counter_read();
9997.092480 |   0)   5.445 us    |    } / ktime_get /
9997.092482 |   0)               |    queue_work_on() {
9997.092484 |   0)   1.852 us    |      clear_pending_if_disabled();
9997.092488 |   0)               |      __queue_work() {
9997.092490 |   0)   1.778 us    |        __rcu_read_lock();
9997.092494 |   0)   1.871 us    |        _raw_spin_lock();
9997.092497 |   0)   2.056 us    |        pwq_tryinc_nr_active();
9997.092501 |   0)   1.797 us    |        get_pwq();
9997.092505 |   0)               |        kick_pool() {
9997.092507 |   0)               |          wake_up_process() {
9997.092509 |   0)               |            try_to_wake_up() {
9997.092511 |   0)   2.130 us    |              _raw_spin_lock_irqsave();
9997.092514 |   0)   2.019 us    |              kthread_is_per_cpu();
9997.092518 |   0)   1.889 us    |              ttwu_queue_wakelist();
9997.092522 |   0)   1.889 us    |              _raw_spin_lock();
9997.092525 |   0)   2.149 us    |              update_rq_clock();
9997.092529 |   0)               |              ttwu_do_activate() {
9997.092531 |   0)               |                enqueue_task() {
9997.092533 |   0)               |                  enqueue_task_fair() {
9997.092535 |   0)               |                    enqueue_entity() {
9997.092537 |   0)   1.944 us    |                      update_curr();
9997.092540 |   0)   2.055 us    |                      __update_load_avg_se();
9997.092544 |   0)   1.982 us    |                      __update_load_avg_cfs_rq();
9997.092548 |   0)               |                      dbs_update_util_handler() {
9997.092550 |   0)   1.870 us    |                        cpufreq_this_cpu_can_update();
9997.092553 |   0)   5.426 us    |                      } / dbs_update_util_handler /
9997.092556 |   0)   1.815 us    |                      update_cfs_group();
9997.092559 |   0)               |                      place_entity() {
9997.092561 |   0)   1.778 us    |                        avg_vruntime();
9997.092564 |   0)   5.333 us    |                      } / place_entity /
9997.092566 |   0)   1.963 us    |                      __enqueue_entity();
9997.092570 |   0) + 35.074 us   |                    } / enqueue_entity /
9997.092572 |   0)   1.889 us    |                    dl_server_update_idle_time();
9997.092576 |   0)   1.796 us    |                    dl_server_start();
9997.092579 |   0)   1.982 us    |                    hrtick_update();
9997.092583 |   0) + 49.833 us   |                  } / enqueue_task_fair /
9997.092584 |   0) + 53.389 us   |                } / enqueue_task /
9997.092586 |   0)               |                wakeup_preempt() {
9997.092588 |   0)   1.852 us    |                  resched_curr();
9997.092591 |   0)   5.352 us    |                } / wakeup_preempt /
9997.092595 |   0) + 65.519 us   |              } / ttwu_do_activate /
9997.092597 |   0)   2.037 us    |              _raw_spin_unlock();
9997.092600 |   0)   1.852 us    |              _raw_spin_unlock_irqrestore();
9997.092604 |   0) + 95.463 us   |            } / try_to_wake_up /
9997.092605 |   0) + 98.889 us   |          } / wake_up_process /
9997.092607 |   0) ! 102.426 us  |        } / kick_pool /
9997.092609 |   0)   1.834 us    |        _raw_spin_unlock();
9997.092612 |   0)   1.778 us    |        __rcu_read_unlock();
9997.092616 |   0) ! 127.833 us  |      } / __queue_work /
9997.092618 |   0) ! 135.315 us  |    } / queue_work_on /
9997.092619 |   0) ! 169.333 us  |  } / button_th_handler [workqueue_button_driver] */

-0    =>   kworker-2578

9997.092682 |   0)               |  button_bh_handler workqueue_button_driver {
9997.092684 |   0)   2.037 us    |    _raw_spin_lock_irqsave();
9997.092688 |   0)   1.944 us    |    _raw_spin_unlock_irqrestore();
9997.092691 |   0)   1.982 us    |    _raw_spin_lock_irqsave();
9997.092698 |   0)   2.167 us    |    _raw_spin_unlock_irqrestore();
9997.092702 |   0)               |    mod_timer() {
9997.092705 |   0)               |      lock_timer_base() {
9997.092707 |   0)   2.370 us    |        _raw_spin_lock_irqsave();
9997.092710 |   0)   6.019 us    |      } /* lock_timer_base /
9997.092713 |   0)   1.852 us    |      detach_if_pending();
9997.092716 |   0)   1.796 us    |      _raw_spin_unlock();
9997.092720 |   0)   2.037 us    |      _raw_spin_lock();
9997.092724 |   0)   1.815 us    |      calc_wheel_index();
9997.092727 |   0)   2.019 us    |      enqueue_timer();
9997.092731 |   0)   2.056 us    |      _raw_spin_unlock_irqrestore();
9997.092735 |   0) + 32.389 us   |    } / mod_timer /
9997.092736 |   0) + 54.481 us   |  } / button_bh_handler [workqueue_button_driver] */

```
</details>


<details><summary>View Tasklet Raw ftrace Log</summary>

```tracer: function_graph

TIME       CPU  DURATION                  FUNCTION CALLS

|          |     |   |                     |   |   |   |

14858.985379 |   0)               |  button_th_handler tasklet_button_driver {
14858.985383 |   0)               |    gpiod_get_value() {
14858.985384 |   0)               |      gpiod_get_raw_value_commit() {
14858.985385 |   0)               |        bcm2835_gpio_get() {
14858.985386 |   0)   1.111 us    |          gpiochip_get_data();
14858.985388 |   0)   2.945 us    |        } /* bcm2835_gpio_get /
14858.985389 |   0)   4.648 us    |      } / gpiod_get_raw_value_commit /
14858.985389 |   0)   6.481 us    |    } / gpiod_get_value /
14858.985390 |   0)               |    ktime_get() {
14858.985391 |   0)   0.592 us    |      arch_counter_read();
14858.985392 |   0)   1.907 us    |    } / ktime_get /
14858.985393 |   0)               |    __tasklet_schedule() {
14858.985394 |   0)   0.648 us    |      __raise_softirq_irqoff();
14858.985395 |   0)   1.907 us    |    } / __tasklet_schedule /
14858.985395 |   0) + 17.612 us   |  } / button_th_handler [tasklet_button_driver] /
14858.985402 |   0)               |  button_bh_handler tasklet_button_driver {
14858.985402 |   0)   0.722 us    |    _raw_spin_lock_irqsave();
14858.985404 |   0)   0.611 us    |    _raw_spin_unlock_irqrestore();
14858.985405 |   0)   0.592 us    |    timer_delete();
14858.985406 |   0)   0.611 us    |    _raw_spin_lock_irqsave();
14858.985407 |   0)   0.593 us    |    _raw_spin_unlock_irqrestore();
14858.985408 |   0)   6.555 us    |  } / button_bh_handler [tasklet_button_driver] /
14859.128416 |   0)               |  button_th_handler tasklet_button_driver {
14859.128418 |   0)               |    gpiod_get_value() {
14859.128418 |   0)               |      gpiod_get_raw_value_commit() {
14859.128419 |   0)               |        bcm2835_gpio_get() {
14859.128420 |   0)   0.667 us    |          gpiochip_get_data();
14859.128421 |   0)   1.852 us    |        } / bcm2835_gpio_get /
14859.128421 |   0)   3.018 us    |      } / gpiod_get_raw_value_commit /
14859.128422 |   0)   4.204 us    |    } / gpiod_get_value /
14859.128423 |   0)               |    ktime_get() {
14859.128423 |   0)   0.592 us    |      arch_counter_read();
14859.128424 |   0)   1.870 us    |    } / ktime_get /
14859.128425 |   0)               |    __tasklet_schedule() {
14859.128426 |   0)   0.666 us    |      __raise_softirq_irqoff();
14859.128427 |   0)   1.777 us    |    } / __tasklet_schedule /
14859.128427 |   0) + 11.500 us   |  } / button_th_handler [tasklet_button_driver] /
14859.128433 |   0)               |  button_bh_handler tasklet_button_driver {
14859.128433 |   0)   0.593 us    |    _raw_spin_lock_irqsave();
14859.128435 |   0)   0.592 us    |    _raw_spin_unlock_irqrestore();
14859.128436 |   0)   0.704 us    |    _raw_spin_lock_irqsave();
14859.128437 |   0)   0.926 us    |    _raw_spin_unlock_irqrestore();
14859.128438 |   0)               |    mod_timer() {
14859.128439 |   0)               |      lock_timer_base() {
14859.128440 |   0)   0.592 us    |        _raw_spin_lock_irqsave();
14859.128441 |   0)   1.926 us    |      } / lock_timer_base /
14859.128442 |   0)   0.556 us    |      detach_if_pending();
14859.128443 |   0)   0.667 us    |      calc_wheel_index();
14859.128445 |   0)   1.000 us    |      enqueue_timer();
14859.128446 |   0)   0.556 us    |      _raw_spin_unlock_irqrestore();
14859.128447 |   0)   8.796 us    |    } / mod_timer /
14859.128448 |   0) + 15.240 us   |  } / button_bh_handler [tasklet_button_driver] */

```
</details>
