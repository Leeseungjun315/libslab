# libslab

> High-Performance Fixed-Size Slab Allocator (Object Cache)
> 16 / 32 / 64 / 128 byte size classes
> Debug-safe • Thread-cache optimized • Systems-level design

---

#  Overview

**libslab**은 고정 크기 객체를 빠르게 할당/해제하기 위한 고성능 Slab Allocator 구현체입니다.

이 libslab은:

- O(1) 고정 크기 클래스 기반 할당
- 페이지 단위 메모리 관리
- Free list 기반 즉시 재사용
- Per-thread cache 지원 (lock contention 감소)
- Guard pattern + Double-free 탐지 (디버그 모드)
- 커널 스타일 설계 구조

를 목표로 설계되었습니다.


이 프로젝트는 **메모리 시스템을 직접 설계하고 구현하는 경험**을 제공하는 시스템 프로그래밍 프로젝트입니다.
그리고 메모리 구조 설계, 동시성 제어, 성능 최적화, 안전성 검증을 모두 포함을 했습니다.

---

#  Architecture

## Memory Hierarchy

Global (size class 16/32/64/128)
    └── Page List
            └── Slots (objects)

---

##  Page Layout

+----------------------------+
| slab_page_t (metadata)     |
+----------------------------+
| bitmap (debug mode only)   |
+----------------------------+
| slot 0                     |
| slot 1                     |
| slot 2                     |
| ...                        |
+----------------------------+

- 각 size class는 여러 개의 page를 가질 수 있음
- 각 page는 고정 크기 slot들로 분할됨
- freelist를 통해 O(1) 재사용

---

##  Slot Layout (SLAB_DEBUG=1)

[obj_hdr]
[guard A (16B)]
[payload (16/32/64/128)]
[guard B (16B)]

---

#  Size Classes

| Enum | Size |
|------|------|
| SLAB_16  | 16 bytes |
| SLAB_32  | 32 bytes |
| SLAB_64  | 64 bytes |
| SLAB_128 | 128 bytes |

---

#  Features

## Fixed-size Object Caching
- Page 내부 freelist 사용
- Constant-time allocation

## Per-thread Cache (Optional)
- TLS 기반 캐시
- Lock contention 감소
- Fast path / Slow path 분리

## Guard Pattern (Redzone)
- Payload 앞/뒤 16 bytes 삽입
- free 시 무결성 검사

## Double-Free Detection
- Slot당 1bit bitmap 관리
- 재해제 시 즉시 abort

## Statistics API
slab_stats_t slab_get_stats(void);

---

#  Build

make

실행:

./build_example \n
./build_test_suite
./build_bench 2000000

---

#  Debug Tests

Double Free Test:
./build_test_doublefree

Overrun Test:
./build_test_overrun

---

# License

MIT License

