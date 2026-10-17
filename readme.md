# SAVE MYSELF: ASHEN BORDER

Windows C++/Direct3D 11 자체 엔진의 싱글플레이 3D 탑다운 아레나 서바이벌. 역병 봉쇄 의식이 실패한 17세기 후반의 가상 국가에서, 귀환한 보급병이 옛 수비대 잔재와 한 초소에서 맞선다. 게임 클라이언트 프로그래머 취업용 포트폴리오다.

## 배포본 실행

[LF17 Windows x64 검증용 ZIP](https://github.com/JellytoCodes/AshenBorder/releases/tag/lf17-20261017-collision)을 받아 전체 압축을 풀고 `AshenBorder.exe`를 실행한다. Windows 10/11 x64·D3D11 기능 수준 11.0 이상·맑은 고딕이 필요하다. 동봉된 DLL과 Resources·Engine 폴더를 함께 둔다. 로그는 `%LOCALAPPDATA%/AshenBorder/Saved/AshenBorder.log`에 남는다.

2026-10-17 같은 PC에서 한글·공백 경로, 무관한 작업 폴더, 실제 NTFS 쓰기 차단 상태의 실행과 필수 파일 누락 3건을 확인했다. 선택 에셋 19묶음·배포 파일 122개이며 MSVC Release DLL 3개를 동봉한다. 첫 LF17 후보는 사용자가 노트북의 Intel Arc 140T에서 초기화·일시정지/재개·정상 종료한 로그를 제공했다. 내부 나무와 줄기·벽 충돌을 반영한 수정본의 새 PC 전체 플레이·승패·재시작은 확인 대기다.

## 빌드와 실행

VS2022 C++ 데스크톱 개발 도구·Windows SDK 10.0.26100.0·Graphics Tools를 사용한다. C++20·v143·Debug/Release x64, MDd/MD이며 PCH는 사용하지 않는다. 한국어 UI에는 Windows의 맑은 고딕 글꼴이 필요하다.

VS에서 AshenBorder.sln을 열고 AshenBorder를 시작 프로젝트로 선택한다. 솔루션에는 Engine과 Client만 포함한다. 명령줄에서는 VS2022 Developer PowerShell을 열고 프로젝트 루트에서 실행한다.

```powershell
msbuild .\AshenBorder.sln /m /p:Configuration=Release /p:Platform=x64
& .\_Local\Binaries\x64\Release\AshenBorder.exe
```

Enter 또는 “시작하기” 버튼으로 시작한다. WASD 이동·자동 공격, 웨이브 종료 후 1~3으로 성장 선택, 1~4로 상점 구매, R로 재추첨, Enter로 다음 웨이브다. 버튼 클릭도 같은 명령을 전달한다. 마지막 웨이브를 버티면 “초소 방어 성공”, 체력이 0이 되면 “전사”가 표시되며 Enter로 다시 시작한다.

단검은 평소 오른손에 들고 다른 무기 공격 중에는 숨긴다. 활은 등에 메고 활 공격 중에만 왼손으로 옮긴다. 투척칼은 몸 주변에 표시하지 않으며, 기존 128슬롯 풀에서 발사된 탄만 그린다. 같은 종류의 여러 슬롯은 장착 외형 하나를 공유하고 공격 규칙은 유지한다.

시작 화면에서는 P를 무시한다. 게임 중 P 일시정지, M 음소거, F6 새 런, F9 자동 공격, F10 그림자, F11 instancing, F12 보수적 정적 컬링(기본 OFF)을 전환한다. F2는 Terrain wireframe, F3는 발·지면 높이/경사 표시다. Esc·창 닫기는 종료다. F4 풀 시험·F7 충돌 시험·시험 모드 F5 배치 재생성·F7의 Space 투사체/Q 근접 판정도 유지한다.

## 현재 상태

2026-10-14 구현을 시작했고 사용자가 LF02 실행을 확인했다. Terrain·스키닝·풀·충돌·자동 전투·웨이브·성장/상점·그림자·가림·HUD와 승패/재시작을 연결했다. 시험 설정은 적 3종·무기 3종·패시브 정의 6종·활성 4종·12초 웨이브 2개다. 자체 제작 캐릭터 4종과 단검·활·투척칼, 초소 환경·무기별 동작·음향을 쓴다. UI 제목은 SAVE MYSELF / ASHEN BORDER, 성장·상점·재화 표시는 “생존 준비”·“야전 정비”·“보급품”이다. 시작 장비는 군용 단검 1개다. 사냥활·투척칼은 상점에서 추가한다. 게임 ID·전투 규칙은 유지한다.

현재 Debug **2,831개**·Release **2,829개**와 각 90프레임 숨김 실행을 통과했다. 내부 나무·줄기와 벽 충돌·수관 통행과 가림은 기준서 5.1.19, 이전 맵 확장은 5.1.18, 무기 손·등 부착은 5.1.17에 기록한다. 별도 `--balance`는 고정 seed의 62사례·63개 검사를 통과했으며 정지 승리도 남는다. 에이전트의 실제 Windows 창 3런 관찰은 사용자 T09 수락과 구분한다. 근거는 기준서 5.1.15·_Local/Saved/Verification/PlayBalance.json, 복사 실행은 SourceFree-PlayBalance.json이다.

v0.23 당시 Debug **2,578개**·Release **2,576개**와 각 90프레임 숨김 실행을 통과했다. Environment 92·Action 122·Character 109·Presentation 392(오디오 72 포함)와 Engine 53파일 보존은 기준서 5.1.14·_Local/Saved/Verification/OutpostPolish.json을 따른다. v0.22의 **2,420/2,418개**, v0.21의 **2,145/2,143개**, v0.20의 **2,026/2,024개**·캐릭터 67개, LF15/16의 **1,920/1,918개**, LF11~LF14의 **1,736/1,734개**는 당시 기록으로 보존한다. 새 초소·무기 동작의 GPU 결과는 _Local/Saved/Verification/OutpostPolish-20261016에 보존한다. [실행 화면](https://chatgpt.com/space/page_22bac242ee888191a06de68f62d05b5c)에 초소 이미지 2장·무기 GIF 3개·음향 미리듣기를 반영했다.

캐릭터 교체 전 LF15는 제안 용량별 정적 최대·초과, 256적/1024탄의 이동 충돌 4회·4096명중/반환과 App 20회 재시작을 검사했다. Release 원본 게임의 **실제 600초** 반복은 별도 20개 검사·exit 0이다. 최대 부하 10분 전투와 구분한다. LF16은 고정 렌더 전후·CPU 14case·짧은 옵션 4조합을 계측했다. CPU 제출과 GPU 시간은 별도이며 전체 게임 FPS가 아니다.

결과는 _Local/Saved/Verification/LF15-LF16-20261016의 구성별 기록과 Soak.json/Soak.csv, _Local/Saved/Performance/LF16에 있다. SourceFree-LF15-LF16.json은 같은 PC에서 원본·Assimp·개발 도구 없는 복사본의 exit 0과 idle.clip 누락 exit 1·원본 불변을 확인했다. LF06~LF14 당시 기록은 보존한다. 사용자 T09 수락·OS 포커스/최소화 수락·가림/캐릭터 외형·음향 실청, 실제 Mixamo·발 IK·새 아트 성능·새 PC 확인은 남아 있다. 독립 Client/T13 결과는 아래 Engine과 Client 항목을 따른다. 저장소는 [JellytoCodes/AshenBorder](https://github.com/JellytoCodes/AshenBorder)다.

## 스트레스와 계측

제작·테스트 자료와 빌드 결과는 `_Local/`에 모으고 폴더 전체를 Git에서 제외한다. Tests·SourceAssets·Scripts·Tools·Libraries/Assimp·agents.md·Binaries·Intermediate·Saved가 그 안에 있다. 아래 명령은 이 로컬 자료가 있을 때 사용한다. 게임 소스·Resources·Engine/Shaders·JSON 헤더와 라이선스는 저장소에 포함한다. 기준서는 _Local/Docs에 보존한다. Visual Studio가 만드는 `.vs/`와 사용자 설정은 별도로 제외한다.

일반 Verify에는 짧은 Stress만 포함된다. 아래 10분 반복과 계측은 따로 실행한다. Before는 변경 전 소스/실행 파일에서 보존하고 After는 변경 후에 실행한다. 다른 빌드·게임·GPU 작업과 동시에 실행하지 않는다. 별도 --balance는 1/60초 tick의 빠른 시뮬레이션이며 실제 시간 플레이·일반 승률·가격 효율 검증은 아니다.

```powershell
.\_Local\Scripts\Verify.ps1 -Configuration Both
& .\_Local\Binaries\x64\Release\RuntimeTests.exe --balance
& .\_Local\Binaries\x64\Release\RuntimeTests.exe --stress
& .\_Local\Binaries\x64\Release\RuntimeTests.exe --soak
.\_Local\Scripts\Measure.ps1 -Phase Before
.\_Local\Scripts\Measure.ps1 -Phase After
.\_Local\Scripts\Measure.ps1 -Phase Options
```

이전 Kenney 조건의 LF16 공식 렌더는 1920×1080·부하 2종별 준비 30초/수집 60초×3회다. Probe와 Options는 짧은 진단이다. F12 컬링은 기본 OFF이며, 현재 옵션 장면에서는 instancing 단독보다 컬링 추가 비용이 컸다.

## Engine과 Client

- Engine은 창·장치·시간·입력, Transform·Camera·Terrain, Model·AnimationClip·Animator, Renderer·UiRenderer·AudioDevice, 풀·Geometry·`SpatialGrid<Key>`를 제공한다. 게임 ID·JSON 스키마·게임 경로를 참조하지 않는다.
- Client의 Main은 앱과 resourceRoot·shaderRoot를 Application에 전달한다. LastFieldRun은 전투·웨이브·보상과 RunProgression을 소유한다. LastFieldApp은 자산·카메라·Environment·HUD·음향을 조립한다. LastFieldHud는 값 사본을 그리고 epoch/선택 ID/상점 revision을 담은 명령을 반환한다. 기존 C++ 이름은 유지한다.
- AssetConverter는 Assimp를 사용하는 독립 오프라인 도구다. Engine·Client 실행에 Assimp DLL은 필요 없다.
- RuntimeTests는 공통 기반·GPU 검사와 실제 Client 조립을 검사하며 Main을 제외한 Client cpp 6개도 컴파일한다.

빌드 의존은 Client→Engine이다. Engine을 다른 Windows/D3D11 게임의 시작점으로 가져가는 프로젝트 템플릿을 목표로 한다. 2026-10-17 원본 Engine 53파일을 그대로 복사해 게임 코드·게임 리소스가 없는 EngineReuseProbe를 만들었다. Engine 단독과 새 Client의 Debug/Release 빌드, 각 90프레임 실행·의도한 초기화/업데이트 실패 종료, 실제 Space 입력·창 크기 변경·Escape 종료를 확인했다. 원본과 복사본 Engine 해시는 같으며, 새 PC의 전체 게임 검증은 별도다.

## 폴더와 include

실제 폴더와 VS 필터를 역할에 맞춰 동일하게 구성했다. `.vcxproj` 파일을 명시적으로 등록하고 `.filters`의 상위 Source 필터까지 맞췄다.

```text
Engine/Source/
  Animation/ App/ Audio/ Core/ Entity/ Graphics/ Resource/ Scene/ Types/ UI/
Client/Source/
  Core/ Game/ Main/ Scene/ UI/
_Local/
  Tests/ SourceAssets/ Scripts/ Tools/ Libraries/Assimp/
  Binaries/ Intermediate/ Saved/ Docs/ Releases/ agents.md
```

각 프로젝트의 Source를 include 루트로 사용한다. 예를 들어 `#include "Graphics/Model.h"`, `#include "Game/LastFieldRun.h"`, `#include "Scene/Environment.h"`로 불러온다. 동명 cpp도 같은 경로 규칙을 따른다. [JellytoStudio](https://github.com/JellytoCodes/Project_JellytoStudio)는 역할 분류를 참고했으며 실제 분석 범위는 기준서 2.17에 있다.

## 캐릭터와 시험 자산

보급병·배회병·척후병·중갑병은 직접 만든 로우폴리 4종이다. 현재 _Local/SourceAssets/ashen-characters/v003은 공통 17본과 v002의 메시·palette·idle/move/attack/death 바이트를 보존하고 플레이어에 무기별 공격 3개를 추가했다. 승인 Fired에서 local SRT × 현재 pose.nodeWorld × characterWorld로 손에 붙이며 판정·발사를 늦추지 않는다. 이전 v001/v002와 [Kenney CC0 원본](https://kenney.nl/assets/mini-characters)은 회귀용으로 보존한다.

- Resources/Generated는 덮어쓰지 않는 generation별 완성 묶음이다. EMSH v1 정적 데이터·EMSH v2 스킨·ECLP v1 클립·텍스처·고지·asset.meta.json을 관리한다.
- Resources/Data/trial-scene.json은 모델·행동/무기별 클립·손 부착·Terrain, trial-game.json schema 2는 전투·웨이브·성장·상점, presentation-scene.json은 표현 모델·음향·나무/장식 배치와 모델 역할별 local 충돌 형상을 지정한다.
- Engine은 모델 묶음의 해시·버전·호환 정보를 확인한다. AudioClip::LoadWav는 RIFF를 검사하며 음향 sidecar/hash는 오프라인·자동 검사에서 대조한다. Client는 실행 중 변환이나 최신 폴더 탐색을 하지 않는다.

에셋 제작 명령에는 `_Local/`의 SourceAssets·Scripts·Tools가 필요하다. 공개된 generation의 파일과 과거 검증 기록은 그대로 보존한다. 정적 OBJ는 BuildTrialAssets.py, Terrain은 BuildTrialTerrainAssets.py, 표현 자산은 BuildPresentationAssets.py로 오프라인 변환한다. 기존 Kenney 원본의 스켈레탈 변환은 _Local/Tools/AssetConverter/AssetConverter.vcxproj를 별도로 빌드한 뒤 Python 3.10 이상에서 다음 명령을 사용한다. 검증된 새 generation을 출력하며 Client 경로는 자동 변경하지 않는다.

```powershell
.\_Local\Scripts\BuildAnimatedAssets.ps1 -Configuration Release -Python python
```

현재 자작 캐릭터·초소·음향은 다음 명령으로 제작하거나 기존 불변 출력을 검증한다. 형상·클립을 바꾸면 새 revision을 사용하며 Client 선택은 자동 변경하지 않는다.

```powershell
python .\_Local\Scripts\BuildAshenCharacters.py --configuration Release --revision v003
python .\_Local\Scripts\BuildAshenOutpost.py --revision v002
python .\_Local\Scripts\BuildAshenMap.py
python .\_Local\Scripts\BuildAshenAudio.py
```

무기·투사체·보급품은 _Local/SourceAssets/ashen-arsenal/v001의 여섯 하위 폴더다. 초소는 나무 44개·장식 27개다. 기존 배치에 내부 나무 12그루와 무너진 벽 3개를 추가했다. 줄기는 원기둥 44개, 벽은 회전 상자 17개로 막고 수관 아래는 통행을 허용한다. Client Environment가 배치와 같은 변환으로 충돌 값을 만들고 게임 Run이 복사해 플레이어·적 이동과 스폰을 검사한다. 수관 페이드는 충돌을 바꾸지 않는다. 시작 위치에서 D로 조금 이동한 뒤 S로 내려가면 중앙 남쪽 나무의 줄기 옆으로 수관 아래를 지나갈 수 있다. 자체 PCM 11역할은 기존 8개와 blade/bow/throwingKnife이며 총성·전자 알림음은 없다. v0.23의 19묶음 복사 실행 exit 0·attack_bow.clip 누락 exit 1·원본 유지는 _Local/Saved/Verification/SourceFree-Outpost.json에 있다. v0.20의 13묶음 SourceFree-AshenBorder.json도 보존한다.

GLB/FBX 실제 일반 샘플을 확인했다. 원본 이름·계층·메시별 inverse bind·초 단위 키를 보존한다. Mixamo 이름 정책은 지정한 프로필에만 적용하며 실제 Mixamo 원본 검증은 남아 있다. Terrain은 65×65·48×48m·간격 0.75m·8,192삼각형이다. 높이와 텍스처 반복 크기, 캐릭터 크기와 카메라 거리·각도는 유지한다. 접지와 충돌은 새 렌더 삼각형 표면을 사용하며 그림자 범위도 맵에 맞춰 넓혔다.

JSON은 nlohmann/json 3.12.0 MIT, 오프라인 임포터는 Assimp 6.0.4 BSD-3-Clause다. JSON 고지는 Libraries에, Assimp 원본과 고지는 _Local/Libraries/Assimp에 보존했다. 외부 pugixml/LICENSE.md는 Markdown 정책의 보존 예외다. 런타임은 D3D11·DXGI·User32·D3DCompiler·WindowsCodecs·Ole32·Shell32·BCrypt·XAudio2를 링크한다.

## 다음 작업과 문서

LF17 배포 후보와 같은 PC의 배포·Engine 재사용 검증을 마쳤다. 다음은 **사용자 새 PC 전체 플레이·승패·재시작 확인**이다. 사용자 T09·외형·가독성·음향 실청 수락과 새 아트 성능은 별도로 남아 있다. 14일은 목표 기간이며 소요 시간을 보장하지 않는다.

- 로컬 개발 기준서: `_Local/Docs/AshenBorder_개발_기준서.docx` (Git 제외) v0.28 — 무기 장착·투척 풀 표시 5.1.17, 배포·Engine 독립 실행 5.1.16, 로컬 관리 폴더·기본 빌드 2.4, 실제 창 조작·밸런스 5.1.15, 제작 1.5·환경 2.7·무기 동작 2.10·UI/PCM 2.15.2. v0.23은 5.1.14, v0.20~v0.22는 5.1.11~5.1.13, LF15/16은 5.1.10·5.9.2·5.10.4~5.10.6에 보존한다.
