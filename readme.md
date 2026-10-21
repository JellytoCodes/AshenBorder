# SAVE MYSELF: ASHEN BORDER

DirectX 11과 C++로 만든 자체 엔진 기반의 싱글플레이 3D 탑다운 서바이벌 게임.
역병 봉쇄 의식이 실패한 17세기 후반, 귀환한 보급병이 초소에 남은 옛 수비대의 잔재와 맞선다.

## 게임

- 군용 단검·사냥활·투척칼을 조합하는 자동 전투
- 전투 사이 성장 선택과 상점 구매를 통한 장비·능력 강화
- 총 20스테이지와 두 차례 보스전, 근접·원거리 적
- 직접 제작한 로우폴리 캐릭터와 환경, 캐릭터를 가리는 나무의 투명화

## 구조

- **기술:** C++20, Direct3D 11, HLSL, XAudio2
- **Engine:** 렌더링·애니메이션·입력·음향·충돌 검사·오브젝트 풀 등 재사용할 공통 기능을 제공한다.
- **Client:** Engine을 조립해 전투·스테이지·성장·상점·카메라·UI를 구현한다.

## 실행

[Windows x64 배포본](https://github.com/JellytoCodes/AshenBorder/releases/latest)을 압축 해제한 뒤 `AshenBorder.exe`를 실행한다. Windows 10/11과 Direct3D 11 기능 수준 11.0 이상 GPU가 필요하다. 동봉된 DLL·Resources·Engine 폴더를 함께 둔다.

| 입력 | 동작 |
|---|---|
| WASD | 이동 |
| Enter | 시작·다음 스테이지·재시작 |
| 1~3 / 1~4 | 성장 선택 / 상점 구매 |
| R | 상점 재추첨 |
| P / M | 일시정지 / 음소거 |
| Esc | 종료 |

## 빌드

Visual Studio 2022의 C++ 데스크톱 개발 도구(v143)와 Windows SDK 10.0.26100.0을 사용한다. VS2022 Developer PowerShell에서 실행한다.

```powershell
msbuild .\AshenBorder.sln /m /p:Configuration=Release /p:Platform=x64
& .\_Local\Binaries\x64\Release\AshenBorder.exe
```
