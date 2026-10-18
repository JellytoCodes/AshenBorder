#include "UI/LastFieldHud.h"

#include "Core/Input.h"
#include "UI/UiData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace
{
    constexpr Engine::Vector4 kPanel{0.025f, 0.04f, 0.065f, 0.96f};
    constexpr Engine::Vector4 kCard{0.055f, 0.09f, 0.13f, 1};
    constexpr Engine::Vector4 kTeal{0.14f, 0.82f, 0.74f, 1};
    constexpr Engine::Vector4 kGold{1.0f, 0.78f, 0.30f, 1};
    constexpr Engine::Vector4 kWhite{0.94f, 0.97f, 1.0f, 1};
    constexpr Engine::Vector4 kDim{0.65f, 0.72f, 0.79f, 1};
    constexpr Engine::Vector4 kTrack{0.12f, 0.17f, 0.22f, 1};
    using Alignment = Engine::UiTextAlignment;

    // Rendering and hit tests share one uniformly scaled 1280x720 design canvas.
    struct Canvas
    {
        float width, height, scale, left, top;
    };

    Canvas MakeCanvas(uint32_t width, uint32_t height)
    {
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float scale = std::min(w / 1280.0f, h / 720.0f);
        return {w, h, scale, (w - 1280.0f * scale) * 0.5f, (h - 720.0f * scale) * 0.5f};
    }

    Engine::UiRect PrimaryButton()
    {
        return {{440, 432}, {400, 52}, kTeal};
    }
    Engine::UiRect AdvanceButton()
    {
        return {{904, 468}, {312, 40}, kTeal};
    }
    Engine::UiRect RerollButton()
    {
        return {{64, 468}, {420, 40}, kGold};
    }
    Engine::UiRect PauseButton()
    {
        return {{950, 80}, {130, 34}, kTrack};
    }
    Engine::UiRect MuteButton()
    {
        return {{1092, 80}, {140, 34}, kTrack};
    }
    Engine::UiRect ChoiceCard(size_t index)
    {
        return {{64 + static_cast<float>(index) * 392, 262}, {368, 206}, kCard};
    }
    Engine::UiRect OfferCard(size_t index)
    {
        return {{64 + static_cast<float>(index) * 294, 262}, {270, 190}, kCard};
    }

    void Panel(Engine::UiFrame& frame, const Canvas& canvas, Engine::UiRect bounds)
    {
        bounds.position = {(canvas.left + bounds.position.x * canvas.scale) / canvas.width,
            (canvas.top + bounds.position.y * canvas.scale) / canvas.height};
        bounds.size = {bounds.size.x * canvas.scale / canvas.width, bounds.size.y * canvas.scale / canvas.height};
        frame.rects.push_back(bounds);
    }

    void Label(Engine::UiFrame& frame, const Canvas& canvas, float x, float y, float height,
        std::string text, Engine::Vector4 color, float maxWidth, Alignment alignment = Alignment::Left)
    {
        frame.texts.push_back({{(canvas.left + x * canvas.scale) / canvas.width,
            (canvas.top + y * canvas.scale) / canvas.height}, height * canvas.scale / canvas.height,
            color, std::move(text), alignment, maxWidth * canvas.scale / canvas.width});
    }

    void Button(Engine::UiFrame& frame, const Canvas& canvas, Engine::UiRect bounds,
        std::string text, bool enabled = true, float height = 20)
    {
        if (!enabled)
        {
            bounds.color = kTrack;
        }
        Panel(frame, canvas, bounds);
        const bool brightBackground = bounds.color.y > 0.6f || bounds.color.x > 0.8f;
        const Engine::Vector4 textColor = !enabled ? kDim :
            (brightBackground ? Engine::Vector4{0.025f, 0.055f, 0.075f, 1} : kWhite);
        Label(frame, canvas, bounds.position.x + bounds.size.x * 0.5f,
            bounds.position.y + (bounds.size.y - height) * 0.5f, height,
            std::move(text), textColor, bounds.size.x - 24, Alignment::Center);
    }

    bool Inside(const Engine::UiRect& bounds, Engine::Vector2 point)
    {
        return point.x >= bounds.position.x && point.y >= bounds.position.y &&
            point.x < bounds.position.x + bounds.size.x && point.y < bounds.position.y + bounds.size.y;
    }

    std::string Number(double value, int precision = 1)
    {
        if (!std::isfinite(value))
        {
            return "?";
        }
        char text[48] = {};
        std::snprintf(text, sizeof(text), "%.*f", precision, value);
        return text;
    }

    float Ratio(double value, double maximum)
    {
        return std::isfinite(value) && std::isfinite(maximum) && maximum > 0 ?
            static_cast<float>(std::clamp(value / maximum, 0.0, 1.0)) : 0;
    }

    const char* WeaponName(Client::WeaponKind kind)
    {
        switch (kind)
        {
        case Client::WeaponKind::Melee: return "군용 단검";
        case Client::WeaponKind::SingleProjectile: return "사냥활";
        case Client::WeaponKind::Shotgun: return "투척칼";
        }
        return "무기";
    }

    const char* PhaseName(Client::WavePhase phase)
    {
        switch (phase)
        {
        case Client::WavePhase::Ready: return "전투 준비";
        case Client::WavePhase::Combat: return "전투";
        case Client::WavePhase::Settlement: return "보상 정산";
        case Client::WavePhase::Rest: return "전투 준비";
        case Client::WavePhase::Complete: return "종료";
        case Client::WavePhase::Selection: return "생존 준비";
        case Client::WavePhase::Shop: return "야전 정비";
        }
        return "상태 확인 중";
    }

    const char* StatName(Client::StatKind stat)
    {
        switch (stat)
        {
        case Client::StatKind::MaxHp: return "최대 체력";
        case Client::StatKind::Speed: return "이동 속도";
        case Client::StatKind::Armor: return "방어력";
        case Client::StatKind::DamageMultiplier: return "피해 배율";
        case Client::StatKind::AttackSpeed: return "공격 속도";
        case Client::StatKind::PickupRadius: return "획득 범위";
        }
        return "능력치";
    }

    const char* PassiveName(const Client::PassiveDefinition& passive)
    {
        if (!passive.modifiers.empty())
        {
            switch (passive.modifiers.front().stat)
            {
            case Client::StatKind::MaxHp: return "비상 식량";
            case Client::StatKind::Speed: return "가죽 장화";
            case Client::StatKind::Armor: return "누빈 조끼";
            case Client::StatKind::DamageMultiplier: return "숫돌";
            case Client::StatKind::AttackSpeed: return "손목 보호대";
            case Client::StatKind::PickupRadius: return "채집 주머니";
            }
        }
        return "보급 장비";
    }

    const char* PassiveDescription(const Client::PassiveDefinition& passive)
    {
        if (!passive.modifiers.empty())
        {
            switch (passive.modifiers.front().stat)
            {
            case Client::StatKind::MaxHp: return "최대 체력을 늘립니다.";
            case Client::StatKind::Speed: return "더 빠르게 이동합니다.";
            case Client::StatKind::Armor: return "받는 피해를 줄입니다.";
            case Client::StatKind::DamageMultiplier: return "무기의 피해량을 높입니다.";
            case Client::StatKind::AttackSpeed: return "공격 간격을 줄입니다.";
            case Client::StatKind::PickupRadius: return "멀리 있는 보상을 줍습니다.";
            }
        }
        return "생존에 필요한 장비입니다.";
    }

    std::string ModifierText(const Client::StatModifierDefinition& modifier)
    {
        const bool percent = modifier.operation == Client::ModifierOperation::Percent;
        const double value = percent ? static_cast<double>(modifier.value) * 100.0 : modifier.value;
        const std::string suffix = percent ? "%" : modifier.stat == Client::StatKind::Speed ? "m/s" :
            modifier.stat == Client::StatKind::PickupRadius ? "m" :
            modifier.stat == Client::StatKind::DamageMultiplier || modifier.stat == Client::StatKind::AttackSpeed ? "배" : "";
        return std::string(StatName(modifier.stat)) + " " + (value >= 0 ? "+" : "") +
            Number(value, !percent && modifier.stat == Client::StatKind::Armor ? 2 : 1) + suffix;
    }

    const char* OfferBlockReason(const Client::HudSnapshot& snapshot, const Client::ShopOffer& offer)
    {
        if (offer.purchased)
        {
            return "교환 완료";
        }
        if (!snapshot.definitions)
        {
            return "정보 없음";
        }
        if (offer.kind == Client::ShopItemKind::Weapon)
        {
            if (!snapshot.definitions->FindWeapon(offer.definitionId))
            {
                return "정보 없음";
            }
            if (snapshot.weapons.size() >= snapshot.definitions->slotLimit)
            {
                return "장비 칸이 가득 참";
            }
        }
        else
        {
            const auto* passive = snapshot.definitions->FindPassive(offer.definitionId);
            if (!passive)
            {
                return "정보 없음";
            }
            const auto stacks = std::count_if(snapshot.progression.passiveSources.begin(),
                snapshot.progression.passiveSources.end(), [passive](const Client::PassiveSource& source)
                {
                    return source.passiveId == passive->id;
                });
            if (static_cast<std::uint64_t>(stacks) >= passive->maxStacks)
            {
                return "더 강화할 수 없음";
            }
        }
        return snapshot.progression.balance < offer.price ? "보급품 부족" : nullptr;
    }

    void PassiveDetails(Engine::UiFrame& frame, const Canvas& canvas,
        const Client::PassiveDefinition* passive, float x, float y, float width)
    {
        if (!passive)
        {
            Label(frame, canvas, x, y, 19, "정보 없음", kDim, width);
            return;
        }
        for (size_t index = 0; index < std::min<size_t>(passive->modifiers.size(), 2); ++index)
        {
            Label(frame, canvas, x, y + static_cast<float>(index) * 28, 20,
                ModifierText(passive->modifiers[index]), kTeal, width);
        }
    }
}

namespace Client
{
    void BuildHud(const HudSnapshot& snapshot, Engine::UiFrame& out, uint32_t width, uint32_t height)
    {
        Engine::UiFrame frame;
        if (width == 0 || height == 0 || (!snapshot.showStart && !snapshot.gameMode))
        {
            out = std::move(frame);
            return;
        }
        const Canvas canvas = MakeCanvas(width, height);
        frame.rects.reserve(64);
        frame.texts.reserve(96);
        if (snapshot.showStart)
        {
            Panel(frame, canvas, {{240, 154}, {800, 360}, kPanel});
            Label(frame, canvas, 640, 184, 48, "SAVE MYSELF", kGold, 704, Alignment::Center);
            Label(frame, canvas, 640, 242, 28, "ASHEN BORDER", kTeal, 704, Alignment::Center);
            const std::string stages = snapshot.definitions ?
                std::to_string(snapshot.definitions->waves.size()) + "개 스테이지를 돌파하고 초소를 지켜내세요" :
                "몰려오는 잔재를 막아내고 초소를 지켜내세요";
            Label(frame, canvas, 640, 294, 22, stages, kWhite, 704, Alignment::Center);
            Label(frame, canvas, 640, 342, 20, "WASD 이동  ·  자동 공격", kDim, 704, Alignment::Center);
            Label(frame, canvas, 640, 374, 18, "전투 중 P 일시정지  ·  M 음소거", kDim, 704, Alignment::Center);
            Button(frame, canvas, PrimaryButton(), "시작하기 [Enter]", true, 23);
            out = std::move(frame);
            return;
        }

        Panel(frame, canvas, {{24, 16}, {1232, 112}, kPanel});
        const bool bossStage = snapshot.wave.bossEnemyId != 0;
        Label(frame, canvas, 48, 32, 22, "스테이지 " + std::to_string(snapshot.wave.waveIndex + 1) +
            " / " + std::to_string(snapshot.wave.waveCount) + "  ·  " + PhaseName(snapshot.wave.phase), kGold, 420);
        Label(frame, canvas, 640, 34, 20, bossStage ? "보스를 쓰러뜨리세요" :
            "남은 시간 " + Number(std::ceil(snapshot.wave.remaining), 0) + "초", kWhite, 260, Alignment::Center);
        Label(frame, canvas, 1232, 32, 22, "보급품 " + std::to_string(snapshot.progression.balance), kGold, 380, Alignment::Right);
        Label(frame, canvas, 48, 72, 20, "체력 " + Number(snapshot.player.hp) + " / " + Number(snapshot.playerStats.maxHp), kWhite, 340);
        Panel(frame, canvas, {{48, 105}, {340, 8}, kTrack});
        Panel(frame, canvas, {{48, 105}, {340 * Ratio(snapshot.player.hp, snapshot.playerStats.maxHp), 8},
            snapshot.player.hp > snapshot.playerStats.maxHp * 0.25f ? kTeal : Engine::Vector4{1, 0.25f, 0.2f, 1}});
        uint64_t levelStartXp = 0;
        if (snapshot.definitions && snapshot.progression.level > 1)
        {
            const size_t index = static_cast<size_t>(snapshot.progression.level - 2);
            if (index < snapshot.definitions->progression.cumulativeXp.size())
            {
                levelStartXp = snapshot.definitions->progression.cumulativeXp[index];
            }
        }
        const uint64_t levelXp = snapshot.progression.acquiredXp >= levelStartXp ? snapshot.progression.acquiredXp - levelStartXp : 0;
        const uint64_t levelTarget = snapshot.progression.nextLevelXp > levelStartXp ? snapshot.progression.nextLevelXp - levelStartXp : 0;
        Label(frame, canvas, 430, 72, 20, "레벨 " + std::to_string(snapshot.progression.level) +
            "  ·  경험치 " + std::to_string(levelXp) + " / " +
            (snapshot.progression.nextLevelXp == 0 ? "최대" : std::to_string(levelTarget)), kWhite, 460);
        Panel(frame, canvas, {{430, 105}, {460, 8}, kTrack});
        Panel(frame, canvas, {{430, 105}, {460 * (snapshot.progression.nextLevelXp == 0 ? 1 :
            Ratio(static_cast<double>(levelXp), static_cast<double>(levelTarget))), 8}, kGold});
        Button(frame, canvas, PauseButton(), snapshot.paused ? "계속하기" : "일시정지", true, 19);
        Button(frame, canvas, MuteButton(), snapshot.muted ? "소리 켜기" : "음소거", true, 19);

        Panel(frame, canvas, {{24, 140}, {1232, 44}, kPanel});
        size_t shown = 0;
        if (snapshot.definitions)
        {
            for (const PassiveDefinition& passive : snapshot.definitions->passives)
            {
                const auto stacks = std::count_if(snapshot.progression.passiveSources.begin(),
                    snapshot.progression.passiveSources.end(), [&passive](const PassiveSource& source)
                    {
                        return source.passiveId == passive.id;
                    });
                if (stacks != 0 && shown < 6)
                {
                    Label(frame, canvas, 44 + static_cast<float>(shown++) * 200, 152, 20,
                        std::string(PassiveName(passive)) + " ×" + std::to_string(stacks), kTeal, 184);
                }
            }
        }
        if (shown == 0)
        {
            Label(frame, canvas, 48, 152, 20, "보유 장비 없음", kDim, 1120);
        }

        if (bossStage && snapshot.wave.bossSpawned && !snapshot.wave.bossDefeated && !snapshot.paused &&
            snapshot.wave.phase == WavePhase::Combat && snapshot.wave.outcome == RunOutcome::Playing &&
            snapshot.progression.phase == ProgressionPhase::Done)
        {
            const std::string name = snapshot.definitions && snapshot.wave.waveIndex < snapshot.definitions->waves.size() ?
                snapshot.definitions->waves[snapshot.wave.waveIndex].bossName : "수비대장";
            Panel(frame, canvas, {{340, 464}, {600, 64}, kPanel});
            Label(frame, canvas, 360, 472, 22, name, kGold, 350);
            Label(frame, canvas, 920, 475, 18, Number(snapshot.wave.bossHp) + " / " +
                Number(snapshot.wave.bossMaxHp), kWhite, 190, Alignment::Right);
            Panel(frame, canvas, {{360, 506}, {560, 10}, kTrack});
            Panel(frame, canvas, {{360, 506}, {560 * Ratio(snapshot.wave.bossHp, snapshot.wave.bossMaxHp), 10},
                {0.85f, 0.22f, 0.18f, 1}});
        }

        Panel(frame, canvas, {{24, 562}, {1232, 34}, kPanel});
        const std::string stats[] = {"이동 " + Number(snapshot.playerStats.speed) + "m/s",
            "방어력 " + Number(snapshot.playerStats.armor, 2), "피해 배율 ×" + Number(snapshot.playerStats.damageMultiplier, 2),
            "공격 속도 ×" + Number(snapshot.playerStats.attackSpeed, 2), "획득 범위 " + Number(snapshot.playerStats.pickupRadius) + "m"};
        for (size_t index = 0; index < 5; ++index)
        {
            Label(frame, canvas, 48 + static_cast<float>(index) * 240, 570, 18, stats[index], kDim, 224);
        }
        Panel(frame, canvas, {{24, 608}, {1232, 96}, kPanel});
        for (size_t index = 0; index < std::min<size_t>(snapshot.weapons.size(), 6); ++index)
        {
            const float x = 44 + static_cast<float>(index) * 202;
            const HudWeaponSlot& slot = snapshot.weapons[index];
            Label(frame, canvas, x, 619, 21, std::to_string(index + 1) + "  " + WeaponName(slot.kind), kGold, 186);
            Label(frame, canvas, x, 648, 18, "공격 간격 " + Number(slot.period) + "초", kDim, 186);
            Label(frame, canvas, x, 673, 18, slot.cooldown > 0 ? "다음 공격 " + Number(slot.cooldown) + "초" : "공격 준비 완료", kWhite, 186);
            Panel(frame, canvas, {{x, 697}, {186, 4}, kTrack});
            Panel(frame, canvas, {{x, 697}, {186 * (1 - Ratio(slot.cooldown, slot.period)), 4}, kTeal});
        }
        if (!snapshot.message.empty())
        {
            Panel(frame, canvas, {{40, 532}, {1200, 26}, kPanel});
            Label(frame, canvas, 640, 535, 20, snapshot.message, kGold, 1152, Alignment::Center);
        }

        if (snapshot.paused || snapshot.wave.outcome != RunOutcome::Playing ||
            snapshot.wave.phase == WavePhase::Complete || snapshot.wave.phase == WavePhase::Ready || snapshot.wave.phase == WavePhase::Rest)
        {
            Panel(frame, canvas, {{320, 216}, {640, 290}, kPanel});
            const bool ended = snapshot.wave.outcome != RunOutcome::Playing || snapshot.wave.phase == WavePhase::Complete;
            Label(frame, canvas, 640, 246, 36, snapshot.paused ? "일시정지" : ended ?
                snapshot.wave.outcome == RunOutcome::Victory ? "초소 방어 성공" : "전사" : "전투 준비", kGold, 560, Alignment::Center);
            if (ended && !snapshot.paused)
            {
                Label(frame, canvas, 640, 316, 23, "레벨 " + std::to_string(snapshot.progression.level) +
                    "  ·  총 경험치 " + std::to_string(snapshot.progression.acquiredXp), kWhite, 560, Alignment::Center);
                Label(frame, canvas, 640, 358, 22, "남은 보급품 " + std::to_string(snapshot.progression.balance), kTeal, 560, Alignment::Center);
            }
            else
            {
                Label(frame, canvas, 640, 328, 22, snapshot.paused ? "준비가 되면 계속하세요." :
                    "WASD 이동  ·  자동 공격", kWhite, 560, Alignment::Center);
            }
            Button(frame, canvas, PrimaryButton(), snapshot.paused ? "계속하기 [P]" : ended ?
                "다시 시작 [Enter]" : "다음 스테이지 [Enter]", true, 23);
        }
        else if (snapshot.progression.phase == ProgressionPhase::Selection)
        {
            Panel(frame, canvas, {{40, 204}, {1200, 318}, kPanel});
            Label(frame, canvas, 64, 220, 28, "생존 준비", kGold, 500);
            Label(frame, canvas, 1216, 226, 20, "장비 하나를 선택하세요. [1~3]", kDim, 650, Alignment::Right);
            const size_t count = std::min<size_t>(snapshot.progression.choices.size(), 3);
            for (size_t index = 0; index < count; ++index)
            {
                const Engine::UiRect bounds = ChoiceCard(index);
                Panel(frame, canvas, bounds);
                const PassiveDefinition* passive = snapshot.definitions ? snapshot.definitions->FindPassive(snapshot.progression.choices[index]) : nullptr;
                const float x = bounds.position.x + 20;
                Label(frame, canvas, x, 278, 27, passive ? PassiveName(*passive) : "정보 없음", kWhite, 298);
                Label(frame, canvas, bounds.position.x + 340, 282, 20, std::to_string(index + 1), kGold, 30, Alignment::Right);
                Label(frame, canvas, x, 318, 20, passive ? PassiveDescription(*passive) : "선택할 수 없는 장비입니다.", kDim, 328);
                PassiveDetails(frame, canvas, passive, x, 356, 328);
                Label(frame, canvas, bounds.position.x + 184, 436, 18, "선택 [" + std::to_string(index + 1) + "]", kGold, 328, Alignment::Center);
            }
            if (count == 0)
            {
                Label(frame, canvas, 640, 316, 24, "선택할 장비가 없습니다.", kWhite, 800, Alignment::Center);
                Button(frame, canvas, AdvanceButton(), "계속하기 [Enter]");
            }
            else
            {
                Label(frame, canvas, 1216, 490, 21, "남은 선택 " + std::to_string(snapshot.progression.pendingChoices), kTeal, 1100, Alignment::Right);
            }
        }
        else if (snapshot.progression.phase == ProgressionPhase::Shop)
        {
            Panel(frame, canvas, {{40, 204}, {1200, 318}, kPanel});
            Label(frame, canvas, 64, 220, 28, "야전 정비", kGold, 500);
            Label(frame, canvas, 1216, 226, 20, "보급품을 장비로 교환하세요. [1~4]", kDim, 650, Alignment::Right);
            for (size_t index = 0; index < std::min<size_t>(snapshot.progression.offers.size(), 4); ++index)
            {
                const ShopOffer& offer = snapshot.progression.offers[index];
                Engine::UiRect bounds = OfferCard(index);
                const char* blocked = OfferBlockReason(snapshot, offer);
                const bool available = blocked == nullptr;
                if (!available)
                {
                    bounds.color = kTrack;
                }
                Panel(frame, canvas, bounds);
                const PassiveDefinition* passive = snapshot.definitions && offer.kind == ShopItemKind::Passive ? snapshot.definitions->FindPassive(offer.definitionId) : nullptr;
                const WeaponDefinition* weapon = snapshot.definitions && offer.kind == ShopItemKind::Weapon ? snapshot.definitions->FindWeapon(offer.definitionId) : nullptr;
                const float x = bounds.position.x + 16;
                Label(frame, canvas, x, 278, 25, passive ? PassiveName(*passive) : weapon ? WeaponName(weapon->kind) : "정보 없음", kWhite, 216);
                Label(frame, canvas, bounds.position.x + 250, 281, 18, std::to_string(index + 1), kGold, 24, Alignment::Right);
                if (passive)
                {
                    Label(frame, canvas, x, 314, 18, PassiveDescription(*passive), kDim, 238);
                    PassiveDetails(frame, canvas, passive, x, 345, 238);
                }
                else if (weapon)
                {
                    const std::string style = weapon->kind == WeaponKind::Melee ? "가까운 적을 베는 무기" :
                        weapon->kind == WeaponKind::SingleProjectile ? "먼 적을 노리는 활" : std::to_string(weapon->projectileCount) + "개 동시 투척";
                    Label(frame, canvas, x, 314, 18, style, kDim, 238);
                    Label(frame, canvas, x, 345, 20, "기본 피해 " + Number(weapon->damage), kTeal, 238);
                    Label(frame, canvas, x, 373, 20, "공격 간격 " + Number(weapon->cooldown) + "초", kTeal, 238);
                }
                if (blocked && !offer.purchased)
                {
                    Label(frame, canvas, bounds.position.x + 250, 397, 18, blocked, kDim, 238, Alignment::Right);
                }
                Label(frame, canvas, bounds.position.x + 250, 418, 22, offer.purchased ? "교환 완료" :
                    "보급품 " + std::to_string(offer.price), available ? kGold : kDim, 238, Alignment::Right);
            }
            const uint64_t cost = snapshot.progression.currentRerollCost;
            Button(frame, canvas, RerollButton(), "다른 물품 [R]  ·  보급품 " + std::to_string(cost),
                snapshot.definitions && snapshot.progression.balance >= cost);
            Button(frame, canvas, AdvanceButton(), "다음 스테이지 [Enter]");
        }
        out = std::move(frame);
    }

    HudCommand ReadHudCommand(const HudSnapshot& snapshot, const Engine::InputState& input, uint32_t width, uint32_t height)
    {
        HudCommand command;
        command.runEpoch = snapshot.runEpoch;
        command.choiceId = snapshot.progression.choiceId;
        command.revision = snapshot.progression.shopRevision;
        if (width == 0 || height == 0)
        {
            return command;
        }
        const Canvas canvas = MakeCanvas(width, height);
        const bool mouseValid = std::isfinite(input.mousePosition.x) && std::isfinite(input.mousePosition.y) &&
            input.mousePosition.x >= 0 && input.mousePosition.y >= 0 && input.mousePosition.x < canvas.width && input.mousePosition.y < canvas.height;
        const Engine::Vector2 mouse{(input.mousePosition.x - canvas.left) / canvas.scale, (input.mousePosition.y - canvas.top) / canvas.scale};
        const bool click = mouseValid && input.IsPressed(Engine::Key::MouseLeft);
        const bool enter = input.IsPressed(Engine::Key::Enter);
        const auto clicked = [click, mouse](const Engine::UiRect& bounds)
        {
            return click && Inside(bounds, mouse);
        };
        if (snapshot.showStart)
        {
            if (enter || clicked(PrimaryButton()))
            {
                command.kind = HudCommandKind::Start;
            }
            return command;
        }
        if (!snapshot.gameMode)
        {
            return command;
        }
        if (clicked(MuteButton()))
        {
            command.kind = HudCommandKind::ToggleMute;
            return command;
        }
        if (clicked(PauseButton()) || (snapshot.paused && clicked(PrimaryButton())))
        {
            command.kind = HudCommandKind::TogglePause;
            return command;
        }
        if (snapshot.paused)
        {
            return command;
        }
        if (snapshot.wave.outcome != RunOutcome::Playing || snapshot.wave.phase == WavePhase::Complete)
        {
            if (enter || clicked(PrimaryButton()))
            {
                command.kind = HudCommandKind::Restart;
            }
            return command;
        }
        if (snapshot.progression.phase == ProgressionPhase::Selection)
        {
            const size_t count = std::min<size_t>(snapshot.progression.choices.size(), 3);
            for (size_t index = 0; index < count; ++index)
            {
                if (input.IsPressed(static_cast<Engine::Key>('1' + index)) || clicked(ChoiceCard(index)))
                {
                    command.kind = HudCommandKind::Choose;
                    command.index = static_cast<uint32_t>(index);
                    return command;
                }
            }
            if (count == 0 && (enter || clicked(AdvanceButton())))
            {
                command.kind = HudCommandKind::Skip;
            }
            return command;
        }
        if (snapshot.progression.phase == ProgressionPhase::Shop)
        {
            const size_t count = std::min<size_t>(snapshot.progression.offers.size(), 4);
            for (size_t index = 0; index < count; ++index)
            {
                const ShopOffer& offer = snapshot.progression.offers[index];
                if (OfferBlockReason(snapshot, offer) == nullptr &&
                    (input.IsPressed(static_cast<Engine::Key>('1' + index)) || clicked(OfferCard(index))))
                {
                    command.kind = HudCommandKind::Buy;
                    command.offerId = offer.offerId;
                    command.index = static_cast<uint32_t>(index);
                    return command;
                }
            }
            const uint64_t cost = snapshot.progression.currentRerollCost;
            if (snapshot.definitions && snapshot.progression.balance >= cost &&
                (input.IsPressed(static_cast<Engine::Key>('R')) || clicked(RerollButton())))
            {
                command.kind = HudCommandKind::Reroll;
            }
            else if (enter || clicked(AdvanceButton()))
            {
                command.kind = HudCommandKind::NextWave;
            }
            return command;
        }
        if ((snapshot.wave.phase == WavePhase::Ready || snapshot.wave.phase == WavePhase::Rest) &&
            (enter || clicked(PrimaryButton())))
        {
            command.kind = HudCommandKind::NextWave;
        }
        return command;
    }
}
