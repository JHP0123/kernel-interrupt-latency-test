## 리눅스 커널 인터럽트 후반부 처리(Bottom-Half) 메커니즘 요약

리눅스 커널의 대표적인 인터럽트 후반부 처리 기법인 **Tasklet, Workqueue, Threaded IRQ**의 동작 방식과 성능을 이론적으로 비교

### 1. 3가지 Bottom-Half(BH) 기법 개요

#### 🔹 Tasklet
- **Top-Half (TH)**: HardIRQ (Interrupt Context)
- **Bottom-Half (BH)**: SoftIRQ (Interrupt Context)
- **동작 방식**: TH에서 BH를 예약하면, 하드웨어 인터럽트 종료 직후 BH에서 즉시 Tasklet 함수(SoftIRQ)가 실행

#### 🔹 Workqueue
- **Top-Half (TH)**: HardIRQ (Interrupt Context)
- **Bottom-Half (BH)**: `kworker` thread (Process Context)
- **동작 방식**: TH에서 작업을 큐(Queue)에 넣고 예약하면, 스케줄러에 의해 `kworker` 스레드가 깨어나 큐에서 작업을 꺼내 수행

#### 🔹 Threaded IRQ
- **Top-Half (TH)**: HardIRQ (Interrupt Context)
- **Bottom-Half (BH)**: `irq/n-이름` kthread (Process Context)
- **동작 방식**: 인터럽트별로 전용 TH와 kthread(BH)를 등록합니다. TH에서 `IRQ_WAKE_THREAD`를 반환하면, 해당 인터럽트에 바인딩된 전용 kthread가 깨어나 비동기적으로 작업을 처리

---

### 2. 기법별 핵심 차이점

#### Threaded IRQ / Workqueue vs Tasklet
* **실행 컨텍스트**: Tasklet의 BH는 **Interrupt Context**에서 실행되는 반면, Threaded IRQ와 Workqueue의 BH는 커널 스레드 기반의 **Process Context**에서 실행

#### Threaded IRQ vs Workqueue
* **Workqueue**: 여러 작업(드라이버)이 **공용 `kworker` 스레드**를 공유. 일반 스케줄링(CFS) 알고리즘을 따르므로 우선순위가 보통이며, 큐에 있는 다른 작업이나 일반 프로세스에 의해 실행이 지연될 수 있음
* **Threaded IRQ**: 인터럽트별로 **전용 kthread(`irq/번호-이름`)**를 독립적으로 가짐. 실시간 스케줄링(RT) 정책을 따르므로 우선순위가 매우 높으며, 공용 `kworker` 스레드보다 먼저 실행되어 지연 시간이 짧음

---

### 3. 컨텍스트 및 멀티코어 동작 특성

#### Context별 Sleep 및 Preemption 규칙
- **Process Context**: 스케줄러의 관리를 받으므로 블로킹 함수 호출 및 자발적 대기(Sleep)가 **가능**하며, 상위 우선순위 스레드나 인터럽트에 의한 선점(Preemption)도 모두 당할 수 있음
- **Interrupt Context**: 스스로 잠드는 Sleep이 **불가능**하며 일반 스레드 선점도 차단. 단, 실행 중 더 높은 우선순위의 외부 HardIRQ가 발생할 경우 일시 정지(Preemption)될 수는 있음

#### SoftIRQ vs Tasklet: 병렬 처리와 동기화 차이
| 구분 | SoftIRQ | Tasklet (SoftIRQ 래핑) |
| :--- | :--- | :--- |
| **설계 목적** | 극한의 성능과 멀티코어 확장성 | 드라이버 개발 편의성 및 락(Lock) 안전성 |
| **코어 병렬성** | **가능** (여러 코어에서 동일 핸들러 동시 실행) | **불가능** (`TASKLET_STATE_RUN` 플래그로 1개 코어만 실행) |
| **락(Lock) 책임** | 개발자 직접 처리 (Per-CPU 변수, Spinlock 필수) | 커널 자동 직렬화 (개발자 락 관리 불필요) |
| **한계 및 단점** | 재진입(Reentrant) 코드 작성 난이도가 매우 높음 | 멀티코어 환경에서 심각한 성능 병목 발생 (**최신 커널에서 Deprecated 되는 원인**) |

#### 3가지 BH 방식의 멀티코어 자원 배분 구조
- **SoftIRQ / Tasklet**: 기본적으로 SoftIRQ는 코어별 독립 실행을 지원하지만, 이를 기반으로 하는 **Tasklet**은 커널 단에서 코어 병렬성을 막아두어 멀티코어 확장이 불가능
- **Workqueue**: 코어별 `kworker` 스레드 풀을 통해 병렬 처리를 지원하지만, 매번 큐에 작업을 삽입하고 워커를 깨우는 문맥 교환(Context Switch) 오버헤드가 상대적으로 큼
- **Threaded IRQ**: 인터럽트(IRQ) 단위로 병렬화. IRQ 번호별로 생성된 전용 스레드가 여러 코어에 분산 스케줄링되므로, 독립적이고 효율적인 병렬화가 가능
