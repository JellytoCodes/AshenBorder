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
        return {{440, 446}, {400, 48}, kTeal};
    }
    Engine::UiRect AdvanceButton()
    {
        return {{834, 490}, {212, 40}, kTeal};
    }
    Engine::UiRect RerollButton()
    {
        return {{234, 490}, {360, 40}, kGold};
    }
    Engine::UiRect PauseButton()
    {
        return {{1198, 28}, {44, 32}, kTrack};
    }
    Engine::UiRect MuteButton()
    {
        return {{314, 510}, {220, 44}, kTrack};
    }
    Engine::UiRect ResumeButton()
    {
        return {{754, 510}, {212, 44}, kTeal};
    }
    Engine::UiRect ChoiceCard(size_t index)
    {
        return {{234 + static_cast<float>(index) * 274, 284}, {262, 148}, kCard};
    }
    Engine::UiRect OfferCard(size_t index)
    {
        return {{234 + static_cast<float>(index) * 206, 280}, {194, 174}, kCard};
    }

    void Panel(Engine::UiFrame& frame, const Canvas& canvas, Engine::UiRect bounds)
    {
        bounds.position = {(canvas.left + bounds.position.x * canvas.scale) / canvas.width,
            (canvas.top + bounds.position.y * canvas.scale) / canvas.height};
        bounds.size = {bounds.size.x * canvas.scale / canvas.width, bounds.size.y * canvas.scale / canvas.height};
        frame.rects.push_back(bounds);
    }

    void HudBackdrop(Engine::UiFrame& frame, const Canvas& canvas, Engine::UiRect bounds)
    {
        bounds.color = {0.025f, 0.04f, 0.065f, 0.38f};
        Panel(frame, canvas, bounds);
        const auto position = bounds.position;
        const auto size = bounds.size;
        constexpr Engine::Vector4 border{0.65f, 0.72f, 0.79f, 0.20f};
        Panel(frame, canvas, {position, {size.x, 1}, border});
        Panel(frame, canvas, {{position.x, position.y + size.y - 1}, {size.x, 1}, border});
        Panel(frame, canvas, {{position.x, position.y + 1}, {1, size.y - 2}, border});
        Panel(frame, canvas, {{position.x + size.x - 1, position.y + 1}, {1, size.y - 2}, border});
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
            return "구매 완료";
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
                return "장착 칸 가득";
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
            Label(frame, canvas, x, y, 18, "정보 없음", kDim, width);
            return;
        }
        for (size_t index = 0; index < std::min<size_t>(passive->modifiers.size(), 2); ++index)
        {
            Label(frame, canvas, x, y + static_cast<float>(index) * 26, 18,
                ModifierText(passive->modifiers[index]), kTeal, width);
        }
    }

    std::string HealthNumber(double value)
    {
        return Number(std::isfinite(value) ? std::ceil(std::max(0.0, value)) : value, 0);
    }

    std::string StageText(const Client::HudSnapshot& snapshot)
    {
        const auto stage = std::min<std::uint64_t>(static_cast<std::uint64_t>(snapshot.wave.waveIndex) + 1,
            snapshot.wave.waveCount);
        return std::to_string(stage) + " / " + std::to_string(snapshot.wave.waveCount);
    }

    std::string WeaponSummary(const Client::HudSnapshot& snapshot)
    {
        std::uint32_t counts[3] = {};
        for (const auto& slot : snapshot.weapons)
        {
            switch (slot.kind)
            {
            case Client::WeaponKind::Melee: ++counts[0]; break;
            case Client::WeaponKind::SingleProjectile: ++counts[1]; break;
            case Client::WeaponKind::Shotgun: ++counts[2]; break;
            }
        }
        const char* names[] = {"단검", "활", "투척칼"};
        std::string summary;
        for (std::size_t index = 0; index < 3; ++index)
        {
            if (counts[index] != 0)
            {
                if (!summary.empty())
                {
                    summary += "  ·  ";
                }
                summary += std::string(names[index]) + " ×" + std::to_string(counts[index]);
            }
        }
        return summary.empty() ? "장착 무기 없음" : summary;
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
        frame.rects.reserve(32);
        frame.texts.reserve(48);
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

        const bool ended = snapshot.wave.outcome != RunOutcome::Playing || snapshot.wave.phase == WavePhase::Complete;
        const bool selection = snapshot.progression.phase == ProgressionPhase::Selection;
        const bool shop = snapshot.progression.phase == ProgressionPhase::Shop;
        const bool ready = snapshot.wave.phase == WavePhase::Ready || snapshot.wave.phase == WavePhase::Rest;
        if (snapshot.paused || ended || selection || shop || ready)
        {
            Panel(frame, canvas, {{0, 0}, {1280, 720}, {0.01f, 0.02f, 0.03f, 0.45f}});
        }
        if (snapshot.paused)
        {
            Panel(frame, canvas, {{290, 154}, {700, 432}, kPanel});
            Label(frame, canvas, 314, 178, 28, "일시정지", kGold, 360);
            Label(frame, canvas, 966, 184, 18, "스테이지 " + StageText(snapshot), kDim, 260, Alignment::Right);
            Label(frame, canvas, 314, 234, 20, "능력", kWhite, 298);
            Label(frame, canvas, 670, 234, 20, "장비", kWhite, 160);
            Label(frame, canvas, 966, 237, 16, "장착 " + std::to_string(snapshot.weapons.size()) + " / " +
                std::to_string(snapshot.definitions ? snapshot.definitions->slotLimit : 6), kDim, 150, Alignment::Right);
            Panel(frame, canvas, {{314, 264}, {298, 1}, kTrack});
            Panel(frame, canvas, {{670, 264}, {296, 1}, kTrack});
            const char* names[] = {"이동 속도", "방어력", "피해 배율", "공격 속도", "획득 범위"};
            const std::string values[] = {Number(snapshot.playerStats.speed) + " m/s", Number(snapshot.playerStats.armor, 2),
                "×" + Number(snapshot.playerStats.damageMultiplier, 2), "×" + Number(snapshot.playerStats.attackSpeed, 2),
                Number(snapshot.playerStats.pickupRadius) + " m"};
            for (size_t index = 0; index < 5; ++index)
            {
                const float y = 280 + static_cast<float>(index) * 34;
                Label(frame, canvas, 314, y, 18, names[index], kDim, 150);
                Label(frame, canvas, 612, y, 18, values[index], kWhite, 138, Alignment::Right);
            }
            Label(frame, canvas, 670, 280, 18, WeaponSummary(snapshot), kWhite, 296);
            size_t shown = 0;
            if (snapshot.definitions)
            {
                for (const auto& passive : snapshot.definitions->passives)
                {
                    const auto stacks = std::count_if(snapshot.progression.passiveSources.begin(),
                        snapshot.progression.passiveSources.end(), [&passive](const PassiveSource& source)
                        {
                            return source.passiveId == passive.id;
                        });
                    if (stacks != 0 && shown < 6)
                    {
                        Label(frame, canvas, 670, 312 + static_cast<float>(shown++) * 26, 17,
                            std::string(PassiveName(passive)) + " ×" + std::to_string(stacks), kTeal, 296);
                    }
                }
            }
            if (shown == 0)
            {
                Label(frame, canvas, 670, 312, 17, "보유 장비 없음", kDim, 296);
            }
            if (!snapshot.message.empty())
            {
                Label(frame, canvas, 640, 476, 16, snapshot.message, kGold, 652, Alignment::Center);
            }
            Button(frame, canvas, MuteButton(), snapshot.muted ? "소리 꺼짐 [M]" : "소리 켜짐 [M]");
            Button(frame, canvas, ResumeButton(), "계속하기 [P]");
            out = std::move(frame);
            return;
        }
        if (ended)
        {
            Panel(frame, canvas, {{380, 190}, {520, 340}, kPanel});
            Label(frame, canvas, 404, 214, 30, snapshot.wave.outcome == RunOutcome::Victory ?
                "초소 방어 완료" : "초소 방어 실패", kGold, 472);
            const char* names[] = {"도달 스테이지", "최종 레벨", "남은 보급품"};
            const std::string values[] = {StageText(snapshot), std::to_string(snapshot.progression.level),
                std::to_string(snapshot.progression.balance)};
            for (size_t index = 0; index < 3; ++index)
            {
                const float y = 282 + static_cast<float>(index) * 42;
                Label(frame, canvas, 404, y, 20, names[index], kDim, 240);
                Label(frame, canvas, 876, y, 20, values[index], kWhite, 200, Alignment::Right);
            }
            Button(frame, canvas, PrimaryButton(), "다시 시작하기 [Enter]", true, 21);
            out = std::move(frame);
            return;
        }
        if (selection)
        {
            Panel(frame, canvas, {{210, 206}, {860, 348}, kPanel});
            Label(frame, canvas, 234, 230, 28, "레벨업", kGold, 400);
            Label(frame, canvas, 1046, 237, 18, "남은 선택 " + std::to_string(snapshot.progression.pendingChoices) +
                "회", kDim, 370, Alignment::Right);
            const size_t count = std::min<size_t>(snapshot.progression.choices.size(), 3);
            for (size_t index = 0; index < count; ++index)
            {
                const auto bounds = ChoiceCard(index);
                Panel(frame, canvas, bounds);
                const auto* passive = snapshot.definitions ? snapshot.definitions->FindPassive(snapshot.progression.choices[index]) : nullptr;
                const float x = bounds.position.x + 16;
                Label(frame, canvas, x, 300, 22, passive ? PassiveName(*passive) : "정보 없음", kWhite, 230);
                PassiveDetails(frame, canvas, passive, x, 340, 230);
                Label(frame, canvas, bounds.position.x + 131, 400, 16, "선택 [" + std::to_string(index + 1) + "]",
                    kGold, 230, Alignment::Center);
            }
            if (count == 0)
            {
                Label(frame, canvas, 640, 322, 22, "받을 수 있는 강화가 없습니다.", kWhite, 800, Alignment::Center);
                Button(frame, canvas, AdvanceButton(), "계속하기 [Enter]");
            }
            if (!snapshot.message.empty())
            {
                Label(frame, canvas, 640, 462, 16, snapshot.message, kGold, 812, Alignment::Center);
            }
            out = std::move(frame);
            return;
        }
        if (shop)
        {
            Panel(frame, canvas, {{210, 166}, {860, 388}, kPanel});
            Label(frame, canvas, 234, 190, 28, "야전 정비", kGold, 480);
            Label(frame, canvas, 1046, 196, 22, "보급품 " + std::to_string(snapshot.progression.balance), kGold, 300, Alignment::Right);
            Label(frame, canvas, 234, 234, 18, "장착 " + std::to_string(snapshot.weapons.size()) + " / " +
                std::to_string(snapshot.definitions ? snapshot.definitions->slotLimit : 6), kDim, 200);
            Label(frame, canvas, 1046, 234, 18, WeaponSummary(snapshot), kDim, 600, Alignment::Right);
            for (size_t index = 0; index < std::min<size_t>(snapshot.progression.offers.size(), 4); ++index)
            {
                const auto& offer = snapshot.progression.offers[index];
                auto bounds = OfferCard(index);
                const char* blocked = OfferBlockReason(snapshot, offer);
                const bool available = blocked == nullptr;
                if (!available)
                {
                    bounds.color = kTrack;
                }
                Panel(frame, canvas, bounds);
                const auto* passive = snapshot.definitions && offer.kind == ShopItemKind::Passive ? snapshot.definitions->FindPassive(offer.definitionId) : nullptr;
                const auto* weapon = snapshot.definitions && offer.kind == ShopItemKind::Weapon ? snapshot.definitions->FindWeapon(offer.definitionId) : nullptr;
                const float x = bounds.position.x + 12;
                const float right = bounds.position.x + 182;
                Label(frame, canvas, x, 296, 21, passive ? PassiveName(*passive) : weapon ? WeaponName(weapon->kind) : "정보 없음",
                    available ? kWhite : kDim, 138);
                Label(frame, canvas, right, 299, 16, "[" + std::to_string(index + 1) + "]", kGold, 28, Alignment::Right);
                if (passive)
                {
                    PassiveDetails(frame, canvas, passive, x, 340, 170);
                }
                else if (weapon)
                {
                    Label(frame, canvas, x, 340, 18, "기본 피해 " + Number(weapon->damage), kTeal, 170);
                    Label(frame, canvas, x, 366, 18, "공격 간격 " + Number(weapon->cooldown) + "초", kTeal, 170);
                }
                if (blocked && !offer.purchased)
                {
                    Label(frame, canvas, right, 400, 16, blocked, kDim, 170, Alignment::Right);
                }
                Label(frame, canvas, right, 426, 18, offer.purchased ? "구매 완료" :
                    "보급품 " + std::to_string(offer.price), available ? kGold : kDim, 170, Alignment::Right);
            }
            if (!snapshot.message.empty())
            {
                Label(frame, canvas, 640, 462, 16, snapshot.message, kGold, 812, Alignment::Center);
            }
            const auto cost = snapshot.progression.currentRerollCost;
            Button(frame, canvas, RerollButton(), "다른 물품 [R] · 보급품 " + std::to_string(cost),
                snapshot.definitions && snapshot.progression.balance >= cost);
            Button(frame, canvas, AdvanceButton(), "다음 스테이지 [Enter]");
            out = std::move(frame);
            return;
        }
        if (ready)
        {
            Panel(frame, canvas, {{380, 190}, {520, 340}, kPanel});
            Label(frame, canvas, 640, 224, 30, "전투 준비", kGold, 472, Alignment::Center);
            Label(frame, canvas, 640, 286, 22, "스테이지 " + StageText(snapshot), kWhite, 472, Alignment::Center);
            Label(frame, canvas, 640, 334, 20, "WASD 이동  ·  자동 공격", kDim, 472, Alignment::Center);
            if (!snapshot.message.empty())
            {
                Label(frame, canvas, 640, 400, 16, snapshot.message, kGold, 472, Alignment::Center);
            }
            Button(frame, canvas, PrimaryButton(), "다음 스테이지 [Enter]", true, 21);
            out = std::move(frame);
            return;
        }

        const bool bossStage = snapshot.wave.bossEnemyId != 0;
        HudBackdrop(frame, canvas, {{30, 20}, {260, 69}, {}});
        Label(frame, canvas, 38, 28, 18, "체력", kWhite, 54);
        Label(frame, canvas, 282, 28, 18, HealthNumber(snapshot.player.hp) + " / " + HealthNumber(snapshot.playerStats.maxHp),
            kWhite, 184, Alignment::Right);
        Panel(frame, canvas, {{38, 54}, {244, 6}, kTrack});
        Panel(frame, canvas, {{38, 54}, {244 * Ratio(snapshot.player.hp, snapshot.playerStats.maxHp), 6},
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
        Label(frame, canvas, 38, 67, 14, "레벨 " + std::to_string(snapshot.progression.level), kDim, 64);
        Panel(frame, canvas, {{112, 72}, {170, 3}, kTrack});
        Panel(frame, canvas, {{112, 72}, {170 * (snapshot.progression.nextLevelXp == 0 ? 1 :
            Ratio(static_cast<double>(levelXp), static_cast<double>(levelTarget))), 3}, kGold});
        HudBackdrop(frame, canvas, {{584, 20}, {112, !bossStage && snapshot.wave.phase == WavePhase::Combat ? 61.0f : 36.0f}, {}});
        Label(frame, canvas, 640, 28, 20, StageText(snapshot), kGold, 96, Alignment::Center);
        if (!bossStage && snapshot.wave.phase == WavePhase::Combat)
        {
            Label(frame, canvas, 640, 55, 18, Number(std::ceil(std::max(0.0, snapshot.wave.remaining)), 0) + "초",
                kWhite, 96, Alignment::Center);
        }
        Button(frame, canvas, PauseButton(), "P", true, 18);
        if (bossStage && snapshot.wave.bossSpawned && !snapshot.wave.bossDefeated &&
            snapshot.wave.phase == WavePhase::Combat && snapshot.progression.phase == ProgressionPhase::Done)
        {
            const std::string name = snapshot.definitions && snapshot.wave.waveIndex < snapshot.definitions->waves.size() ?
                snapshot.definitions->waves[snapshot.wave.waveIndex].bossName : "수비대장";
            HudBackdrop(frame, canvas, {{389, 93}, {502, 32}, {}});
            Label(frame, canvas, 397, 101, 16, name, kGold, 160);
            Panel(frame, canvas, {{569, 110}, {314, 3}, kTrack});
            Panel(frame, canvas, {{569, 110}, {314 * Ratio(snapshot.wave.bossHp, snapshot.wave.bossMaxHp), 3},
                {0.85f, 0.22f, 0.18f, 1}});
        }
        HudBackdrop(frame, canvas, {{886, 666}, {364, 34}, {}});
        Label(frame, canvas, 1242, 674, 18, snapshot.weapons.empty() ? "장착 무기 없음" :
            "장착  " + WeaponSummary(snapshot), kWhite, 348, Alignment::Right);
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
        if (snapshot.paused)
        {
            if (clicked(MuteButton()))
            {
                command.kind = HudCommandKind::ToggleMute;
            }
            else if (clicked(ResumeButton()))
            {
                command.kind = HudCommandKind::TogglePause;
            }
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
        else if (snapshot.wave.phase == WavePhase::Combat && clicked(PauseButton()))
        {
            command.kind = HudCommandKind::TogglePause;
        }
        return command;
    }
}
