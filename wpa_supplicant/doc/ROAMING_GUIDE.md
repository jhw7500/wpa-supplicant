# wpa_supplicant 로밍(Roaming) 가이드

## 목차

1. [개요](#1-개요)
2. [로밍 아키텍처](#2-로밍-아키텍처)
3. [로밍 조건](#3-로밍-조건)
4. [로밍 설정](#4-로밍-설정)
5. [bgscan과 로밍 관계](#5-bgscan과-로밍-관계)
6. [로밍 이벤트](#6-로밍-이벤트)
7. [설정 예시](#7-설정-예시)

---

## 1. 개요

wpa_supplicant의 로밍(Roaming)은 **더 나은 AP로 자동 재연결**하는 기능입니다.

```
┌─────────────────────────────────────────────────────────────────────┐
│                        wpa_supplicant 로밍 아키텍처                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   이벤트 기반 로밍 (events.c)                                      │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │                  비콘 손실 (beacon_loss)                │   │
│   │                  ▼                       즉시 로밍 트리거      │   │
│   │                                          │   │   │   │
│   │         ┌────────────────────────────────────────────────┐   │   │
│   │         │ 로밍 조건 검사 (need_to_roam)    │   │   │   │
│   │         │ 6가지 조건 검사                    │   │   │   │
│   │         │                                    │   │   │   │
│   │         └────────────────────────────────────────────┘   │   │   │   │
│   │                                             │   │   │   │
│   │                  ▼                       ▼               ▼       │   │
│   │            신호 강도/보안 등급 우선순위           │   ▼   새 AP 선택 │   │   │
│   │                                          │   │   │   │
│   └────────────────────────────────────────────────────┘   │   │   │   │
│   ▼                                                   │   ▼       │   │
│                    드라이버 재연결 요청                 │   │       │   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. 로밍 아키텍처

### 핵심 파일

| 파일 | 역할 |
|------|------|
| `wpa_supplicant/bss.c` | BSS 선택 로직 (신호, 보안, SNR, 우선순위 기반) |
| `wpa_supplicant/events.c` | 이벤트 기반 로밍 트리거 (비콘 손실, 신호 변화) |
| `wpa_supplicant/hs20_supplicant.c` | HS20 로밍 콘소리움 선택 |
| `wpa_supplicant/config_ssid.h` | 로밍 관련 설정 구조체 |

### 로밍 함수 흐름

```
이벤트 발생 (driver)
        │
        ▼
wpa_supplicant_event()
        │
        ├── wpas_notify_beacon_loss()  → 이벤트 처리
        ├── wpas_notify_signal_change() → 이벤트 처리
        │
        ▼
    events.c
        │
        ▼
wpas_need_to_roam()
        │
        ├── wpa_supplicant_need_to_roam_within_ess()
        │   │   #ifdef CONFIG_NO_ROAMING
        │   │   └── 로밍 조건 검사
        │   #endif
        │
        └── BSS 선택 후 드라이버에 재연결 요청
        │
        ▼
    wpa_supplicant_select_bss()
        │   │   bss.c
        │   └── 신호, 보안, SNR, 우선순위 기반 AP 선택
        │
        ▼
    드라이버 재연결 요청
```

---

## 3. 로밍 조건

### 3.1 조건 검사 함수 (events.c: 1865-2011행)

```c
static int wpa_supplicant_need_to_roam_within_ess(
    struct wpa_supplicant *wpa_s,
    struct wpa_bss *current_bss,
    struct wpa_bss *selected)
{
    int min_diff, diff;
    int to_5ghz;  // 5GHz 대역 로밍 여부

    // ===== 조건 1: 5GHz 대역 로밍 금지 =====
    to_5ghz = selected->freq > 4000 && current_bss->freq < 4000;

    // ===== 조건 2: 신호 강도 조건 =====
    if (cur_level < 0 && cur_level > selected->level + to_5ghz * 2) {
        // 현재 신호가 선택된 BSS보다 2dB 이상 좋으면
        wpa_dbg("Skip roam - Current BSS has better signal level");
        return 0;  // 로밍 안 함
    }

    // ===== 조건 3: 예상 처리량 조건 =====
    if (cur_est > sel_est + 5000) {
        // 현재 예상 처리량이 5Mbps 이상 높으면
        wpa_dbg("Skip roam - Current BSS has better estimated throughput");
        return 0;  // 로밍 안 함
    }

    // ===== 조건 4: SNR 조건 =====
    if (cur_snr > GREAT_SNR) {
        // 현재 SNR가 우수(GREAT_SNR) 이상이면
        wpa_dbg("Skip roam - Current BSS has good SNR");
        return 0;  // 로밍 안 함
    }

    // ===== 조건 5: 신호 차이 조건 =====
    if (to_5ghz)
        min_diff -= 2;  // 5GHz 대역 로밍은 2dB 완화 허용

    diff = selected->level - cur_level;
    if (diff < min_diff) {
        // 신호 차이가 최소 허용보다 작으면
        wpa_dbg("Skip roam - too small difference in signal level");
        return 0;  // 로밍 안 함
    } else {
        wpa_dbg("Allow reassociation due to difference in signal level");
        return 1;  // 로밍 허용!
    }

    // ===== 조건 6: 낮은 신호 조건 =====
    if (cur_level < -85) {  // -86 dBm .. -85 dBm
        wpa_dbg("Allow reassociation due to difference in signal level");
        return 1;  // 로밍 허용!
    }
}
```

### 3.2 조건 요약

| 번호 | 조건 | 설명 | 로밍 |
|------|------|------|:----|
| 1 | 5GHz 대역 | 선택된 BSS가 5GHz이고 현재는 2.4GHz 이하일 때만 | ❌ |
| 2 | 신호 강도 | 현재 BSS가 선택된 BSS보다 2dB 이상 좋으면 | ❌ |
| 3 | 예상 처리량 | 현재 예상 처리량이 선택된 BSS보다 5Mbps 이상 높으면 | ❌ |
| 4 | SNR | 현재 SNR가 우수(GREAT_SNR) 이상이면 | ❌ |
| 5 | 신호 차이 | 신호 차이가 최소 허용(min_diff)보다 작으면 | ❌ |
| 6 | 낮은 신호 | 현재 신호가 -86dBm 미만이면 | ✅ |

---

## 4. 로밍 설정

### 4.1 로밍 금지 설정

```bash
# 기본값: -80dBm 이하에서만 로밍 (대부 드라이버 기준)
disallow_lower_ess_roaming=-80

# 로밍 억제 (1로 설정 시 로밍 금지)
disallow_lower_ess_roaming=1

# 5GHz 대역 로밍 억제
disallow_5ghz_roaming=1
```

### 4.2 BSS 관련 설정

```bash
# BSS 테이블 크기 제한
bss_count=10

# BSS 만료 시간 (초)
bss_expiration_age=300

# BSS 업데이트 주기 (초)
bss_update_period=10

# BSS 만료 안함
bss_expire_age=0

# BSS 만료 수 제한
bss_max_count=0
```

### 4.3 스캔 결과 만료 설정

```bash
# 스캔 결과 만료 주파수 제한 (MHz)
scan_res_freq_limit=10

# 스캔 결과 유효 시간 (초)
scan_res_max_age=120

# 스캔 결과 플러시
scan_res_flush=0
```

---

## 5. bgscan과 로밍 관계

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   bgscan과 로밍의 관계                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   bgscan (백그라운드 스캔)                                         │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │ 스캔 결과 수집          │                                     │
│   │ • 주파수 정보 업데이트  │                                     │
│   │ • 이웃 AP 관계 저장(learn) │                                     │
│   └──────────────────────────────────────────────────────────────────────┘   │
│                           │                                       │
│                           ▼                                       │
│   ┌───────────────────────────────────────────────────────────────────────┐   │
│   │ BSS 선택 로직 (bss.c)          │                                     │
│   │ • 신호, 보안, SNR 기반        │                                     │
│   │ • 로밍 조건 검사                │                                     │
│   └───────────────────────────────────────────────────────────────────────┘   │
│                           │                                       │
│                           ▼                                       │
│   ┌───────────────────────────────────────────────────────────────────────┐   │
│   │ 로밍 조건 검사 (events.c)    │                                     │
│   │ • 6가지 조건 분석              │                                     │
│   │ • 드라이버 재연결 요청          │                                     │
│   └───────────────────────────────────────────────────────────────────────┘   │
│                           │                                       │
│                           ▼                                       │
│   ┌───────────────────────────────────────────────────────────────────────┐   │
│   │ 드라이버 재연결                   │                                     │
│   │ • 로밍 실행                      │                                     │
│   └───────────────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 역할 분담

| 구분 | bgscan | BSS 선택 | 이벤트 처리 |
|------|--------|----------|----------|
| **역할** | 스캔 데이터 수집 | 로밍 대상 AP 선택 | 로밍 트리거/실행 |
| **수행 위치** | `bgscan_simple.c`, `bgscan_learn.c` | `bss.c` | `events.c` |
| **조건 검사** | 신호 강도 기반 간격 조정 | `need_to_roam()` 조건 검사 |
| **재연결** | 드라이버에 재연결 요청 | 드라이버 재연결 요청 |

---

## 6. 로밍 이벤트

### 6.1 비콘 손실 이벤트

```c
// events.c: 비콘 손실 감지
wpas_notify_beacon_loss()
{
    // TODO: speed up background scanning
}
```

### 6.2 신호 변화 이벤트

```c
// events.c: 신호 강도 모니터링 등록
wpa_drv_signal_monitor(..., threshold, 4)
{
    // 신호 강도가 threshold 미만/초과로 변화하면
    wpas_notify_signal_change(..., above, current_signal, ...)
}
```

### 6.3 로밍 트리거 순서

```
비콘 손실 감지
        │
        ▼
wpas_notify_beacon_loss()
        │
        ▼
wpas_need_to_roam()
        │   • 조건 검사 (6가지)
        │   • 새 AP 선택
        │
        ▼
wpa_supplicant_deauthenticate()
wpa_supplicant_select_bss()
wpa_supplicant_associate()
```

---

## 7. 설정 예시

### 7.1 기본 로밍 설정

```bash
network={
    ssid="CorporateNetwork"
    psk="password"

    # 로밍 금지: -75dBm 이상에서만 로밍
    disallow_lower_ess_roaming=-75

    # BSS 설정
    bss_count=10
    bss_expiration_age=300

    # bgscan (신호 기반 간격 조정)
    bgscan="simple:30:-70:300"

    # 우선순위 (높을수록 우선)
    priority=2
}
```

### 7.2 5GHz 대역 로밍 억제

```bash
network={
    ssid="CorporateNetwork"
    psk="password"

    # 5GHz 대역 로밍 억제
    disallow_5ghz_roaming=1

    # 5GHz에서 2.4GHz로 로밍 금지
    disallow_lower_ess_roaming=-70
}
```

### 7.3 learn 모드 사용

```bash
network={
    ssid="CorporateNetwork"
    psk="password"

    # learn 모드 (BSS/이웃 정보 학습)
    bgscan="learn:30:-70:300:/var/lib/wpa_bgscan.dat"

    # HS20 로밍 콘소리움 선택
    roaming_consortium_selection=dddf:01:23:45:67:89:ab:cd:ef:01:23:45:67:89:ab
}
```

### 7.4 로밍 억제 설정

```bash
# 로밍 억제 (disallow_lower_ess_roaming=1)
network={
    ssid="CorporateNetwork"
    psk="password"
    disallow_lower_ess_roaming=1
    priority=2
}

# 기본값 (대부 드라이버 기준)
network={
    ssid="PublicNetwork"
    psk="password"
    # -80dBm 이하에서만 로밍
    disallow_lower_ess_roaming=-80
}
```

---

## 참조

### 핵심 파일

- `wpa_supplicant/bss.c` - BSS 선택 로직
- `wpa_supplicant/events.c` - 이벤트 기반 로밍 트리거
- `wpa_supplicant/hs20_supplicant.c` - HS20 로밍 콘소리움
- `wpa_supplicant/config_ssid.h` - 로밍 관련 설정 구조체

### 관련 문서

- [bgscan 분석 문서](BGSCAN_ROAMING_ANALYSIS.md)
- [wpa_supplicant README](../README)
