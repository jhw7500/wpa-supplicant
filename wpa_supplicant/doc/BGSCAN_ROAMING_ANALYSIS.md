# wpa_supplicant bgscan 로밍 로직 상세 분석

## 목차

1. [아키텍처 개요](#1-아키텍처-개요)
2. [설정 파일 구조](#2-설정-파일-구조)
3. [simple 모드 상세 분석](#3-simple-모드-상세-분석)
4. [learn 모드 상세 분석](#4-learn-모드-상세-분석)
5. [메인 호출 플로우](#5-메인-호출-플로우)
6. [로밍 트리거 조건](#6-로밍-트리거-조건)
7. [설정 예시](#7-설정-예시)
8. [모드 비교](#8-모드-비교)

---

## 1. 아키텍처 개요

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         wpa_supplicant bgscan 아키텍처                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐    ┌─────────────┐    ┌──────────────────────────────┐   │
│  │  설정 파일    │───▶│ config.c    │───▶│    bgscan.c (공통 인터페이스)  │   │
│  │ bgscan 옵션  │    │ (파싱)      │    │                              │   │
│  └──────────────┘    └─────────────┘    │  • bgscan_init()             │   │
│                                         │  • bgscan_deinit()            │   │
│                                         │  • bgscan_notify_scan()       │   │
│                                         │  • bgscan_notify_signal_...   │   │
│                                         └───────────┬──────────────────┘   │
│                                                     │                     │
│                           ┌─────────────────────────┼─────────────────┐   │
│                           ▼                         ▼                 │   │
│                  ┌──────────────────┐     ┌──────────────────┐         │   │
│                  │ bgscan_simple.c  │     │ bgscan_learn.c   │         │   │
│                  │ (단순 알고리즘)  │     │ (학습 알고리즘)  │         │   │
│                  ├──────────────────┤     ├──────────────────┤         │   │
│                  │ 신호 임계값 기반 │     │ BSS 토폴로지 학습│         │   │
│                  │ 짧은/긴 스캔 간격│     │ 이웃 AP 관계 저장│         │   │
│                  └──────────────────┘     └──────────────────┘         │   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.1 핵심 파일

| 파일 | 설명 |
|------|------|
| `wpa_supplicant/bgscan.h` | bgscan 모듈 인터페이스 정의 |
| `wpa_supplicant/bgscan.c` | bgscan 공통 초기화/이벤트 처리 |
| `wpa_supplicant/bgscan_simple.c` | simple 알고리즘 구현 |
| `wpa_supplicant/bgscan_learn.c` | learn 알고리즘 구현 |
| `wpa_supplicant/config.c` | 설정 파일 파싱 (bgscan 옵션) |
| `wpa_supplicant/wpa_supplicant.c` | 메인 호출 플로우 |

### 1.2 데이터 흐름

```
연결 완료 → bgscan_init() → 타이머 등록 → 주기적 스캔
                                    │
                                    ├─ 스캔 완료 → bgscan_notify_scan()
                                    │
                                    ├─ 신호 변화 → bgscan_notify_signal_change()
                                    │
                                    └─ 비콘 손실 → bgscan_notify_beacon_loss()
```

---

## 2. 설정 파일 구조

### 2.1 config_ssid.h 구조체

```c
struct wpa_ssid {
    /**
     * bgscan - Background scan and roaming parameters or %NULL if none
     *
     * This is an optional set of parameters for background scanning and
     * roaming within a network (ESS) in following format:
     * <bgscan module name>:<module parameters>
     */
    char *bgscan;
    // ...
};
```

### 2.2 설정 파일 포맷

#### simple 모드
```
bgscan="simple:<short_interval>:<signal_threshold>:<long_interval>"
```

#### learn 모드
```
bgscan="learn:<short_interval>:<signal_threshold>:<long_interval>[:<file_path>]"
```

### 2.3 파라미터 설명

| 파라미터 | 타입 | 설명 | 기본값 |
|----------|------|------|--------|
| `short_interval` | 정수 (초) | 신호가 임계값 미만일 때 스캔 간격 | 30 |
| `signal_threshold` | 정수 (dBm) | 신호 강도 임계값 (0 = 비활성화) | 0 |
| `long_interval` | 정수 (초) | 신호가 임계값 이상일 때 스캔 간격 | 30 |
| `file_path` | 문자열 | learn 모드 학습 데이터 저장 경로 | 없음 |

---

## 3. simple 모드 상세 분석

### 3.1 데이터 구조

```c
struct bgscan_simple_data {
    struct wpa_supplicant *wpa_s;
    const struct wpa_ssid *ssid;
    int scan_interval;           // 현재 스캔 간격
    int signal_threshold;        // 신호 임계값
    int short_scan_count;        // 짧은 간격 스캔 횟수
    int max_short_scans;         // 최대 짧은 스캔 횟수
    int short_interval;          // 신호 < 임계값일 때 간격
    int long_interval;           // 신호 > 임계값일 때 간격
    struct os_reltime last_bgscan; // 마지막 스캔 시간
};
```

### 3.2 초기화 로직

```c
static void * bgscan_simple_init(struct wpa_supplicant *wpa_s,
                                 const char *params,
                                 const struct wpa_ssid *ssid)
{
    // 1. 파라미터 파싱
    data->short_interval = atoi(params);           // 첫 번째 파라미터
    data->signal_threshold = atoi(pos);            // 두 번째 파라미터
    data->long_interval = atoi(pos);               // 세 번째 파라미터

    // 2. 기본값 설정
    if (data->short_interval <= 0)  data->short_interval = 30;
    if (data->long_interval <= 0)   data->long_interval = 30;

    // 3. 최대 짧은 스캔 횟수 계산
    data->max_short_scans = data->long_interval / data->short_interval + 1;

    // 4. 신호 모니터링 등록 (CQM - Connection Quality Monitoring)
    wpa_drv_signal_monitor(wpa_s, data->signal_threshold, 4);

    // 5. 초기 스캔 간격 결정
    data->scan_interval = data->short_interval;  // 기본값
    if (wpa_drv_signal_poll(wpa_s, &siginfo) == 0 &&
        siginfo.current_signal >= data->signal_threshold)
        data->scan_interval = data->long_interval;  // 신호가 좋으면 긴 간격

    // 6. 타이머 등록
    eloop_register_timeout(data->scan_interval, 0,
                           bgscan_simple_timeout, data, NULL);
}
```

### 3.3 스캔 타임아웃 로직

```
┌─────────────────────────────────────────────────────────────────────┐
│                       bgscan_simple_timeout                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 스캔 파라미터 설정                                              │
│     • SSID: 연결된 네트워크의 SSID                                   │
│     • freqs: scan_freq (설정된 주파수 리스트)                        │
│                                                                     │
│  2. 스캔 트리거                                                    │
│     ┌─────────────────┐                                           │
│     │ wpa_supplicant_ │                                             │
│     │   trigger_scan  │                                             │
│     └────────┬────────┘                                             │
│              │                                                      │
│              ▼                                                      │
│     ┌─────────────────┐    ┌─────────────────┐                      │
│     │ 성공시          │    │ 실패시          │                      │
│     │ • short_interval│    │ • 타이머 재등록  │                      │
│     │   사용 시 카운트│    │   (같은 간격)   │                      │
│     │   증가          │    │                 │                      │
│     │ • 카운트 >= max │    │                 │                      │
│     │   이면 long로   │    │                 │                      │
│     │   전환          │    │                 │                      │
│     │ • long_interval │    │                 │                      │
│     │   사용 시 카운트│    │                 │                      │
│     │   감소          │    │                 │                      │
│     └─────────────────┘    └─────────────────┘                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.4 신호 변화 처리

```c
static void bgscan_simple_notify_signal_change(void *priv, int above,
                                               int current_signal,
                                               int current_noise,
                                               int current_txrate)
{
    // above: 1=임계값 초과, 0=임계값 미만

    // 상태 1: 긴 간격 사용 중 → 신호가 임계값 미만으로 떨어짐
    if (scan_interval == long_interval && !above) {
        scan_interval = short_interval;  // 짧은 간격으로 전환

        // 즉시 스캔 조건: 1초 이전에 스캔했고 예산이 남았을 때
        if (now.sec > last_bgscan.sec + 1 && short_scan_count <= max_short_scans)
            scan = 1;  // 즉시 스캔
    }

    // 상태 2: 짧은 간격 사용 중 → 신호가 임계값 초과로 회복
    else if (scan_interval == short_interval && above) {
        scan_interval = long_interval;  // 긴 간격으로 전환
        eloop_cancel_timeout(...);      // 타이머 재설정
    }

    // 상태 3: 이미 임계값 미만인데 신호가 추가로 4dB 하락
    else if (!above) {
        if (now.sec > last_bgscan.sec + 10)  // 10초 이상 경과
            scan = 1;  // 즉시 스캔
    }
}
```

### 3.5 스캔 상태 머신

```
                    ┌─────────────────────────────────────────┐
                    │           신호 강도 모니터링             │
                    │        (wpa_drv_signal_monitor)         │
                    └──────────────────┬──────────────────────┘
                                       │
                ┌──────────────────────┼──────────────────────┐
                │                      │                      │
                ▼                      ▼                      ▼
         ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
         │   초기 상태  │        │ SHORT 상태  │        │  LONG 상태  │
         │             │        │  (신호 약함)│        │  (신호 강함)│
         │ • init 타이머│        │             │        │             │
         │ • 신호 폴링  │        │ • 간격: 짧음│        │ • 간격: 김 │
         └──────┬──────┘        │ • 카운트 관리│        │ • 카운트 감소│
                │               └──────┬──────┘        └──────┬──────┘
                │                      │                      │
                │          ┌───────────┴───────────┐          │
                │          │                       │          │
                │          ▼                       ▼          │
                │    신호 > threshold        신호 < threshold │
                │    (CQM 이벤트 above=1)    (CQM 이벤트 above=0)│
                │          │                       │          │
                │          │                       │          │
                │          ▼                       ▼          │
                │    ┌─────────────┐        ┌─────────────┐    │
                └───▶│ LONG로 전환  │        │ SHORT로 전환 │    │
                     │ • 타이머 재설정│       │ • 즉시 스캔 │    │
                     │ • 카운트 감소  │       │   (조건부)  │    │
                     └──────────────────────────────────────────┘
```

---

## 4. learn 모드 상세 분석

### 4.1 데이터 구조

```c
struct bgscan_learn_bss {
    struct dl_list list;
    u8 bssid[ETH_ALEN];     // BSS MAC 주소
    int freq;                // 주파수
    u8 *neigh;               // 이웃 AP 목록
    size_t num_neigh;        // 이웃 AP 수
};

struct bgscan_learn_data {
    struct wpa_supplicant *wpa_s;
    const struct wpa_ssid *ssid;
    int scan_interval;
    int signal_threshold;
    int short_interval;
    int long_interval;
    struct os_reltime last_bgscan;
    char *fname;             // 학습 데이터 파일 경로
    struct dl_list bss;      // BSS 리스트
    int *supp_freqs;         // 지원되는 주파수 목록
    int probe_idx;           // 프로빙 주파수 인덱스
};
```

### 4.2 BSS 토폴로지 학습

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         learn 모드 BSS 토폴로지 학습                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   AP1 (2412 MHz) ─────┬────── AP2 (2437 MHz)                               │
│          │            │            │                                        │
│          └───────┬────┴────────────┘                                        │
│                    │                                                         │
│                    ▼                                                         │
│            스캔 결과 분석                                                   │
│            • 각 AP의 주파수 학습                                            │
│            • AP 간 이웃 관계 학습                                           │
│                                                                             │
│   학습 데이터 저장 (파일 형식)                                               │
│   ┌─────────────────────────────────────────────────────────────────────┐  │
│   │ wpa_supplicant-bgscan-learn                                          │  │
│   │ BSS aa:bb:cc:dd:ee:ff 2412                                           │  │
│   │ BSS 11:22:33:44:55:66 2437                                           │  │
│   │ NEIGHBOR aa:bb:cc:dd:ee:ff 11:22:33:44:55:66                         │  │
│   │ NEIGHBOR aa:bb:cc:dd:ee:ff 77:88:99:aa:bb:cc                         │  │
│   └─────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│   스캔 최적화                                                                │
│   • 학습된 주파수만 스캔                                                    │
│   • 순환적으로 새 주파수 프로빙                                              │
│   • 이웃 관계를 이용한 효율적 로밍                                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 스캔 알고리즘

```c
static void bgscan_learn_timeout(void *eloop_ctx, void *timeout_ctx)
{
    // 1. 학습된 주파수 가져오기
    freqs = bgscan_learn_get_freqs(data, &count);

    // 2. 새 주파수 프로빙 (supp_freqs에서 순환)
    freqs = bgscan_learn_get_probe_freq(data, freqs, count);

    // 3. 스캔 요청
    params.freqs = freqs;  // 학습된 주파수 + 프로빙 주파수
    wpa_supplicant_trigger_scan(wpa_s, &params);
}
```

### 4.4 스캔 결과 처리

```c
static int bgscan_learn_notify_scan(void *priv,
                                    struct wpa_scan_results *scan_res)
{
    // 1. 모든 BSS 수집 (최대 50개)
    for (i = 0; i < scan_res->num; i++) {
        if (bgscan_learn_bss_match(data, res)) {
            // SSID가 일치하는 BSS만 수집
            bssid[num_bssid++] = res->bssid;
        }
    }

    // 2. BSS 정보 업데이트/추가
    for (i = 0; i < scan_res->num; i++) {
        bss = bgscan_learn_get_bss(data, res->bssid);
        if (bss && bss->freq != res->freq) {
            bss->freq = res->freq;  // 주파수 업데이트
        } else if (!bss) {
            // 새 BSS 추가
            bss = os_zalloc(sizeof(*bss));
            os_memcpy(bss->bssid, res->bssid, ETH_ALEN);
            bss->freq = res->freq;
            dl_list_add(&data->bss, &bss->list);
        }

        // 3. 이웃 관계 학습
        for (j = 0; j < num_bssid; j++) {
            bgscan_learn_add_neighbor(bss, bssid[j]);
        }
    }
}
```

---

## 5. 메인 호출 플로우

### 5.1 wpa_supplicant.c 호출 시점

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          wpa_supplicant bgscan 호출                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. 연결 완료 시점                                                          │
│     wpa_supplicant_connected()                                             │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_deinit()             // 기존 bgscan 정리                         │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_init(wpa_s, ssid, name)  // 새 bgscan 초기화                     │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_notify_scan()          // 초기 스캔 결과 전달                     │
│                                                                             │
│  2. 스캔 완료 시점                                                          │
│     wpa_supplicant_event_dispatch()                                        │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_notify_scan(wpa_s, scan_res)                                     │
│                                                                             │
│  3. 신호 변화 감지 (CQM 이벤트)                                              │
│     wpa_drv_event()                                                        │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_notify_signal_change()                                          │
│                                                                             │
│  4. 비콘 손실 감지                                                          │
│     wpa_supplicant_event()                                                 │
│          │                                                                  │
│          ▼                                                                  │
│     bgscan_notify_beacon_loss()                                            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 5.2 함수 호출 관계

```c
// wpa_supplicant.c
wpa_supplicant_connected()
└── bgscan_init(wpa_s, ssid, bgscan_name)
    ├── bgscan_simple_init() / bgscan_learn_init()
    └── bgscan_notify_scan(wpa_s, scan_res)

// 스캔 완료 시
wpa_supplicant_event()
└── bgscan_notify_scan(wpa_s, scan_res)
    └── bgscan_simple_ops.notify_scan() / bgscan_learn_ops.notify_scan()

// CQM (Connection Quality Monitoring) 이벤트
wpa_drv_event()
└── bgscan_notify_signal_change()
    └── bgscan_simple_ops.notify_signal_change() / bgscan_learn_ops.notify_signal_change()
```

---

## 6. 로밍 트리거 조건

### 6.1 신호 기반 트리거

| 조건 | 동작 | 비고 |
|------|------|------|
| 신호 < threshold | short_interval로 전환 | 즉시 스캔 가능 |
| 신호 > threshold | long_interval로 전환 | 타이머 재설정 |
| 신호 추가 하락 4dB | 즉시 스캔 | 10초 이상 경과 시 |

### 6.2 스캔 카운트 기반 백오프

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          스캔 카운트 기반 백오프                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   short_scan_count < max_short_scans                                       │
│          │                                                                  │
│          ▼                                                                  │
│   short_interval 유지                                                       │
│   • 빈번한 스캔으로 빠른 로밍                                              │
│                                                                             │
│   short_scan_count >= max_short_scans                                      │
│          │                                                                  │
│          ▼                                                                  │
│   long_interval로 전환                                                      │
│   • 무선 리소스 절약                                                        │
│   • max_short_scans = long_interval / short_interval + 1                   │
│                                                                             │
│   예: short=30, long=300                                                    │
│       → max_short_scans = 300/30 + 1 = 11                                  │
│       → 11번 짧은 스캔 후 긴 간격으로 전환                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.3 타이밍 다이어그램

```
신호 강도    ──┐                ┌────────┐                  ┌─────────
(dBm)        │  │                │        │                  │         │
        -60  │  │                │        │                  │         │
        -65  ├──┘                │        │                  │         │
              │                  │        │                  │         │
        -70  │                  │        ├────────┐          │         │
              │                  │        │        │          │         │
        -75  │                  │        │        ├──────────┘         │
              └──────────────────┴────────┴────────┴──────────────────┴───▶  시간
              │                  │        │        │          │
              │                  │        │        │          │
상태          │ LONG    │ SHORT  │ SHORT  │ SHORT  │ SHORT   │ LONG
              │         │        │        │        │          │
스캔 간격     │  300초  │ 30초   │ 30초   │ 30초   │  30초    │ 300초
              │         │        │        │        │          │
CQM 이벤트    │  above  │ below  │ below   │ below  │ above    │ above
```

---

## 7. 설정 예시

### 7.1 simple 모드 예시

```
network={
    ssid="MyNetwork"
    psk="password"

    # 기본 설정: 30초/300초, -65dBm 임계값
    bgscan="simple:30:-65:300"

    # 빈번한 스캔: 15초/60초, -70dBm 임계값
    bgscan="simple:15:-70:60"

    # 신호 모니터링 없이 고정 간격
    bgscan="simple:30:0:30"

    # 공공장소 (빠른 로밍)
    bgscan="simple:10:-75:30"
}
```

### 7.2 learn 모드 예시

```
network={
    ssid="EnterpriseNetwork"
    psk="password"

    # 학습 모드: 30초/300초, -70dBm 임계값, 데이터 파일 저장
    bgscan="learn:30:-70:300:/etc/wpa_bgscan_learn.dat"

    # 대형 캠퍼스 (학습 파일 사용)
    bgscan="learn:20:-65:180:/var/lib/wpa_supplicant/bgscan_learn"
}
```

### 7.3 파라미터 튜닝 가이드

| 환경 | short_interval | long_interval | signal_threshold |
|------|----------------|---------------|------------------|
| 사무실 (안정적) | 30-60 | 300-600 | -70 ~ -75 |
| 공공장소 (이동 많음) | 10-20 | 30-60 | -65 ~ -70 |
| 공장 (간섭 많음) | 15-30 | 60-120 | -60 ~ -65 |
| 가정 | 30-60 | 300-600 | -75 ~ -80 |

---

## 8. 모드 비교

| 특성 | simple 모드 | learn 모드 |
|------|-------------|-----------|
| 복잡도 | 낮음 | 높음 |
| 메모리 사용 | 낮음 (~100바이트) | 높음 (BSS/이웃 정보) |
| 스캔 효율 | 모든 주파수 스캔 | 학습된 주파수만 스캔 |
| 파일 I/O | 없음 | 학습 데이터 저장/로드 |
| 적용 환경 | 단일 AP/단순 환경 | 대형 ESS/다중 AP 환경 |
| 로밍 속도 | 빠름 (신호 기반) | 중간 (학습 필요) |
| 배터리 소모 | 높음 (자주 스캔) | 낮음 (최적화된 스캔) |

---

## 9. 디버깅

### 9.1 로그 메시지

```
# 초기화
bgscan: Initialized module 'simple' with parameters '30:-65:300'
bgscan simple: Signal strength threshold -65  Short bgscan interval 30  Long bgscan interval 300
bgscan simple: Init scan interval: 30

# 스캔 요청
bgscan simple: Request a background scan
bgscan learn: Request a background scan
bgscan learn: Scanning frequencies: 2412 2437 2462

# 신호 변화
bgscan simple: signal level changed (above=0 current_signal=-70 ...)
bgscan simple: Start using short bgscan interval
bgscan simple: Trigger immediate scan

# 스캔 결과
bgscan simple: scan result notification
bgscan learn: 5 matching BSSes in scan results
bgscan learn: Add BSS aa:bb:cc:dd:ee:ff freq=2412
```

### 9.2 디버깅 명령어

```bash
# 로그 레벨 설정
wpa_cli -i wlan0 log_level DEBUG

# bgscan 상태 확인
wpa_cli -i wlan0 status

# 신호 정보 확인
wpa_cli -i wlan0 signal_monitor

# 수동 스캔
wpa_cli -i wlan0 scan
wpa_cli -i wlan0 scan_results
```

---

## 10. 참조

### 10.1 소스 파일

- `wpa_supplicant/bgscan.h` - 인터페이스 정의
- `wpa_supplicant/bgscan.c` - 공통 구현
- `wpa_supplicant/bgscan_simple.c` - simple 모듈
- `wpa_supplicant/bgscan_learn.c` - learn 모듈
- `wpa_supplicant/config.c` - 설정 파싱
- `wpa_supplicant/wpa_supplicant.c` - 메인 호출

### 10.2 관련 문서

- [wpa_supplicant README](../README)
- [wpa_supplicant 설정 예시](wpa_supplicant.conf)
- [Linux Wireless CQM Documentation](https://wireless.wiki.kernel.org/en/developers/Documentation/cqm)

---

*문서 버전: 1.0*
*작성일: 2026-02-12*
*wpa_supplicant 버전: 2.10*
