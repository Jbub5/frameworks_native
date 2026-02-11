/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"

#include <chrono>
#include <cmath>
#include <deque>
#include <map>

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <common/trace.h>
#include <ftl/enum.h>
#include <ftl/fake_guard.h>
#include <ftl/match.h>
#include <ftl/unit.h>
#include <scheduler/Fps.h>
#include <scheduler/FrameRateMode.h>

#include "RefreshRateSelector.h"

#include <com_android_graphics_surfaceflinger_flags.h>

#undef LOG_TAG
#define LOG_TAG "RefreshRateSelector"

namespace android::scheduler {
namespace {

using namespace com::android::graphics::surfaceflinger;

struct RefreshRateScore {
    FrameRateMode frameRateMode;
    float overallScore;
    struct {
        float modeBelowThreshold;
        float modeAboveThreshold;
    } fixedRateBelowThresholdLayersScore;
};

constexpr RefreshRateSelector::GlobalSignals kNoSignals;

std::vector<Fps> constructKnownFrameRates(const DisplayModes& modes) {
    (void)modes;
    return {60_Hz};
}

std::vector<DisplayModeIterator> sortByRefreshRate(const DisplayModes& modes) {
    std::vector<DisplayModeIterator> sortedModes;
    sortedModes.reserve(modes.size());
    for (auto it = modes.begin(); it != modes.end(); ++it) {
        sortedModes.push_back(it);
    }

    std::sort(sortedModes.begin(), sortedModes.end(), [](auto it1, auto it2) {
        const auto& mode1 = it1->second;
        const auto& mode2 = it2->second;

        if (mode1->getVsyncRate().getPeriodNsecs() == mode2->getVsyncRate().getPeriodNsecs()) {
            return mode1->getGroup() > mode2->getGroup();
        }

        return mode1->getVsyncRate().getPeriodNsecs() > mode2->getVsyncRate().getPeriodNsecs();
    });

    return sortedModes;
}

std::pair<unsigned, unsigned> divisorRange(Fps vsyncRate, Fps peakFps, FpsRange range,
                                           RefreshRateSelector::Config::FrameRateOverride config) {
    if (config != RefreshRateSelector::Config::FrameRateOverride::Enabled) {
        return {1, 1};
    }

    using fps_approx_ops::operator/;
    // use signed type as `fps / range.max` might be 0
    auto start = std::max(1, static_cast<int>(peakFps / range.max) - 1);
    if (FlagManager::getInstance().vrr_config()) {
        start = std::max(1,
                         static_cast<int>(vsyncRate /
                                          std::min(range.max, peakFps, fps_approx_ops::operator<)) -
                                 1);
    }
    const auto end = vsyncRate /
            std::max(range.min, RefreshRateSelector::kMinSupportedFrameRate,
                     fps_approx_ops::operator<);

    return {start, end};
}

bool shouldEnableFrameRateOverride(const std::vector<DisplayModeIterator>& sortedModes) {
    for (const auto it1 : sortedModes) {
        const auto& mode1 = it1->second;
        for (const auto it2 : sortedModes) {
            const auto& mode2 = it2->second;

            if (RefreshRateSelector::getFrameRateDivisor(mode1->getPeakFps(),
                                                         mode2->getPeakFps()) >= 2) {
                return true;
            }
        }
    }
    return false;
}

std::string toString(const RefreshRateSelector::PolicyVariant& policy) {
    using namespace std::string_literals;

    return ftl::match(
            policy,
            [](const RefreshRateSelector::DisplayManagerPolicy& policy) {
                return "DisplayManagerPolicy"s + policy.toString();
            },
            [](const RefreshRateSelector::OverridePolicy& policy) {
                return "OverridePolicy"s + policy.toString();
            },
            [](RefreshRateSelector::NoOverridePolicy) { return "NoOverridePolicy"s; });
}

} // namespace

auto RefreshRateSelector::createFrameRateModes(
        const Policy& policy, std::function<bool(const DisplayMode&)>&& filterModes,
        const FpsRange& renderRange) const -> std::vector<FrameRateMode> {
    struct Key {
        Fps fps;
        int32_t group;
    };

    struct KeyLess {
        bool operator()(const Key& a, const Key& b) const {
            using namespace fps_approx_ops;
            if (a.fps != b.fps) {
                return a.fps < b.fps;
            }

            // For the same fps the order doesn't really matter, but we still
            // want the behaviour of a strictly less operator.
            // We use the group id as the secondary ordering for that.
            return a.group < b.group;
        }
    };

    std::map<Key, DisplayModeIterator, KeyLess> ratesMap;
    for (auto it = mDisplayModes.begin(); it != mDisplayModes.end(); ++it) {
        const auto& [id, mode] = *it;

        if (!filterModes(*mode)) {
            continue;
        }
        const auto vsyncRate = mode->getVsyncRate();
        const auto peakFps = mode->getPeakFps();
        const auto [start, end] =
                divisorRange(vsyncRate, peakFps, renderRange, mConfig.enableFrameRateOverride);
        for (auto divisor = start; divisor <= end; divisor++) {
            const auto fps = vsyncRate / divisor;
            using fps_approx_ops::operator<;
            if (divisor > 1 && fps < kMinSupportedFrameRate) {
                break;
            }

            if (mConfig.enableFrameRateOverride == Config::FrameRateOverride::Enabled &&
                !renderRange.includes(fps)) {
                continue;
            }

            if (mConfig.enableFrameRateOverride ==
                        Config::FrameRateOverride::AppOverrideNativeRefreshRates &&
                !isNativeRefreshRate(fps)) {
                continue;
            }

            const auto [existingIter, emplaceHappened] =
                    ratesMap.try_emplace(Key{fps, mode->getGroup()}, it);
            if (emplaceHappened) {
                ALOGV("%s: including %s (%s(%s))", __func__, to_string(fps).c_str(),
                      to_string(peakFps).c_str(), to_string(vsyncRate).c_str());
            } else {
                // If the primary physical range is a single rate, prefer to stay in that rate
                // even if there is a lower physical refresh rate available. This would cause more
                // cases to stay within the primary physical range
                const Fps existingModeFps = existingIter->second->second->getPeakFps();
                const bool existingModeIsPrimaryRange = policy.primaryRangeIsSingleRate() &&
                        policy.primaryRanges.physical.includes(existingModeFps);
                const bool newModeIsPrimaryRange = policy.primaryRangeIsSingleRate() &&
                        policy.primaryRanges.physical.includes(mode->getPeakFps());
                if (newModeIsPrimaryRange == existingModeIsPrimaryRange) {
                    // We might need to update the map as we found a lower refresh rate
                    if (isStrictlyLess(mode->getPeakFps(), existingModeFps)) {
                        existingIter->second = it;
                        ALOGV("%s: changing %s (%s(%s)) as we found a lower physical rate",
                              __func__, to_string(fps).c_str(), to_string(peakFps).c_str(),
                              to_string(vsyncRate).c_str());
                    }
                } else if (newModeIsPrimaryRange) {
                    existingIter->second = it;
                    ALOGV("%s: changing %s (%s(%s)) to stay in the primary range", __func__,
                          to_string(fps).c_str(), to_string(peakFps).c_str(),
                          to_string(vsyncRate).c_str());
                }
            }
        }
    }

    std::vector<FrameRateMode> frameRateModes;
    frameRateModes.reserve(ratesMap.size());
    for (const auto& [key, mode] : ratesMap) {
        frameRateModes.emplace_back(FrameRateMode{key.fps, ftl::as_non_null(mode->second)});
    }

    // We always want that the lowest frame rate will be corresponding to the
    // lowest mode for power saving.
    const auto lowestRefreshRateIt =
            std::min_element(frameRateModes.begin(), frameRateModes.end(),
                             [](const FrameRateMode& lhs, const FrameRateMode& rhs) {
                                 return isStrictlyLess(lhs.modePtr->getVsyncRate(),
                                                       rhs.modePtr->getVsyncRate());
                             });
    frameRateModes.erase(frameRateModes.begin(), lowestRefreshRateIt);

    return frameRateModes;
}

struct RefreshRateSelector::RefreshRateScoreComparator {
    bool operator()(const RefreshRateScore& lhs, const RefreshRateScore& rhs) const {
        const auto& [frameRateMode, overallScore, _] = lhs;

        std::string name = to_string(frameRateMode);

        ALOGV("%s sorting scores %.2f", name.c_str(), overallScore);

        if (!ScoredFrameRate::scoresEqual(overallScore, rhs.overallScore)) {
            return overallScore > rhs.overallScore;
        }

        if (refreshRateOrder == RefreshRateOrder::Descending) {
            using fps_approx_ops::operator>;
            return frameRateMode.fps > rhs.frameRateMode.fps;
        } else {
            using fps_approx_ops::operator<;
            return frameRateMode.fps < rhs.frameRateMode.fps;
        }
    }

    const RefreshRateOrder refreshRateOrder;
};

std::string RefreshRateSelector::Policy::toString() const {
    return base::StringPrintf("{defaultModeId=%d, allowGroupSwitching=%s"
                              ", primaryRanges=%s, appRequestRanges=%s idleScreenConfig=%s}",
                              ftl::to_underlying(defaultMode),
                              allowGroupSwitching ? "true" : "false",
                              to_string(primaryRanges).c_str(), to_string(appRequestRanges).c_str(),
                              idleScreenConfigOpt ? idleScreenConfigOpt->toString().c_str()
                                                  : "nullptr");
}

std::pair<nsecs_t, nsecs_t> RefreshRateSelector::getDisplayFrames(nsecs_t layerPeriod,
                                                                  nsecs_t displayPeriod) const {
    auto [quotient, remainder] = std::div(layerPeriod, displayPeriod);
    if (remainder <= MARGIN_FOR_PERIOD_CALCULATION ||
        std::abs(remainder - displayPeriod) <= MARGIN_FOR_PERIOD_CALCULATION) {
        quotient++;
        remainder = 0;
    }

    return {quotient, remainder};
}

float RefreshRateSelector::calculateNonExactMatchingDefaultLayerScoreLocked(
        nsecs_t displayPeriod, nsecs_t layerPeriod) const {
    // Find the actual rate the layer will render, assuming
    // that layerPeriod is the minimal period to render a frame.
    // For example if layerPeriod is 20ms and displayPeriod is 16ms,
    // then the actualLayerPeriod will be 32ms, because it is the
    // smallest multiple of the display period which is >= layerPeriod.
    auto actualLayerPeriod = displayPeriod;
    int multiplier = 1;
    while (layerPeriod > actualLayerPeriod + MARGIN_FOR_PERIOD_CALCULATION) {
        multiplier++;
        actualLayerPeriod = displayPeriod * multiplier;
    }

    // Because of the threshold we used above it's possible that score is slightly
    // above 1.
    return std::min(1.0f, static_cast<float>(layerPeriod) / static_cast<float>(actualLayerPeriod));
}

float RefreshRateSelector::calculateNonExactMatchingLayerScoreLocked(const LayerRequirement& layer,
                                                                     Fps refreshRate) const {
    constexpr float kScoreForFractionalPairs = .8f;

    const auto displayPeriod = refreshRate.getPeriodNsecs();
    const auto layerPeriod = layer.desiredRefreshRate.getPeriodNsecs();
    if (layer.vote == LayerVoteType::ExplicitDefault) {
        return calculateNonExactMatchingDefaultLayerScoreLocked(displayPeriod, layerPeriod);
    }

    if (layer.vote == LayerVoteType::ExplicitGte) {
        using fps_approx_ops::operator>=;
        if (refreshRate >= layer.desiredRefreshRate) {
            return 1.0f;
        } else {
            return calculateDistanceScoreLocked(layer.desiredRefreshRate, refreshRate);
        }
    }

    if (layer.vote == LayerVoteType::ExplicitExactOrMultiple ||
        layer.vote == LayerVoteType::Heuristic) {
        using fps_approx_ops::operator<;
        if (refreshRate < 60_Hz) {
            const bool favorsAtLeast60 =
                    std::find_if(mFrameRatesThatFavorsAtLeast60.begin(),
                                 mFrameRatesThatFavorsAtLeast60.end(), [&](Fps fps) {
                                     using fps_approx_ops::operator==;
                                     return fps == layer.desiredRefreshRate;
                                 }) != mFrameRatesThatFavorsAtLeast60.end();
            if (favorsAtLeast60) {
                return 0;
            }
        }

        const float multiplier = refreshRate.getValue() / layer.desiredRefreshRate.getValue();

        // We only want to score this layer as a fractional pair if the content is not
        // significantly faster than the display rate, at it would cause a significant frame drop.
        // It is more appropriate to choose a higher display rate even if
        // a pull-down will be required.
        constexpr float kMinMultiplier = 0.75f;
        if (multiplier >= kMinMultiplier &&
            isFractionalPairOrMultiple(refreshRate, layer.desiredRefreshRate)) {
            return kScoreForFractionalPairs;
        }

        // Calculate how many display vsyncs we need to present a single frame for this
        // layer
        const auto [displayFramesQuotient, displayFramesRemainder] =
                getDisplayFrames(layerPeriod, displayPeriod);
        static constexpr size_t MAX_FRAMES_TO_FIT = 10; // Stop calculating when score < 0.1
        if (displayFramesRemainder == 0) {
            // Layer desired refresh rate matches the display rate.
            return 1.0f;
        }

        if (displayFramesQuotient == 0) {
            // Layer desired refresh rate is higher than the display rate.
            return (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                    (1.0f / (MAX_FRAMES_TO_FIT + 1));
        }

        // Layer desired refresh rate is lower than the display rate. Check how well it fits
        // the cadence.
        auto diff = std::abs(displayFramesRemainder - (displayPeriod - displayFramesRemainder));
        int iter = 2;
        while (diff > MARGIN_FOR_PERIOD_CALCULATION && iter < MAX_FRAMES_TO_FIT) {
            diff = diff - (displayPeriod - diff);
            iter++;
        }

        return (1.0f / iter);
    }

    return 0;
}

float RefreshRateSelector::calculateDistanceScoreLocked(Fps referenceRate, Fps refreshRate) const {
    using fps_approx_ops::operator>=;
    const float ratio = referenceRate >= refreshRate
            ? refreshRate.getValue() / referenceRate.getValue()
            : referenceRate.getValue() / refreshRate.getValue();
    // Use ratio^2 to get a lower score the more we get further from the reference rate.
    return ratio * ratio;
}

float RefreshRateSelector::calculateDistanceScoreFromMaxLocked(Fps refreshRate) const {
    const auto& maxFps = mAppRequestFrameRates.back().fps;
    return calculateDistanceScoreLocked(maxFps, refreshRate);
}

float RefreshRateSelector::calculateLayerScoreLocked(const LayerRequirement& layer, Fps refreshRate,
                                                     bool isSeamlessSwitch) const {
    (void)layer;
    (void)refreshRate;
    (void)isSeamlessSwitch;
    return 1.0f;
}

auto RefreshRateSelector::getRankedFrameRates(const std::vector<LayerRequirement>& layers,
                                              GlobalSignals signals, Fps pacesetterFps) const
        -> RankedFrameRates {
    GetRankedFrameRatesCache cache{layers, signals, pacesetterFps};

    std::lock_guard lock(mLock);

    if (mGetRankedFrameRatesCache && mGetRankedFrameRatesCache->matches(cache)) {
        return mGetRankedFrameRatesCache->result;
    }

    cache.result = getRankedFrameRatesLocked(layers, signals, pacesetterFps);
    mGetRankedFrameRatesCache = std::move(cache);
    return mGetRankedFrameRatesCache->result;
}

using LayerRequirementPtrs = std::vector<const RefreshRateSelector::LayerRequirement*>;
using PerUidLayerRequirements = std::unordered_map<uid_t, LayerRequirementPtrs>;

PerUidLayerRequirements groupLayersByUid(
        const std::vector<RefreshRateSelector::LayerRequirement>& layers) {
    PerUidLayerRequirements layersByUid;
    for (const auto& layer : layers) {
        const auto it = layersByUid.emplace(layer.ownerUid, LayerRequirementPtrs()).first;
        auto& layersWithSameUid = it->second;
        layersWithSameUid.push_back(&layer);
    }
    return layersByUid;
}

auto RefreshRateSelector::getRankedFrameRatesLocked(const std::vector<LayerRequirement>& layers,
                                                    GlobalSignals signals, Fps pacesetterFps) const
        -> RankedFrameRates {
    (void)layers;
    (void)signals;
    (void)pacesetterFps;

    FrameRateRanking ranking;

    const auto& activeMode = *getActiveModeLocked().modePtr;

    ranking.emplace_back(ScoredFrameRate{
        FrameRateMode{activeMode.getPeakFps(), ftl::as_non_null(mDisplayModes.get(activeMode.getId())->get())},
        1.0f
    });

    return {ranking, kNoSignals};
}

auto RefreshRateSelector::getFrameRateOverrides(const std::vector<LayerRequirement>& layers,
                                                Fps displayRefreshRate,
                                                GlobalSignals globalSignals) const
        -> UidToFrameRateOverride {
    (void)layers;
    (void)displayRefreshRate;
    (void)globalSignals;

    return {};
}

ftl::Optional<FrameRateMode> RefreshRateSelector::onKernelTimerChanged(
        ftl::Optional<DisplayModeId> desiredModeIdOpt, bool timerExpired) const {
    std::lock_guard lock(mLock);

    const auto current =
            desiredModeIdOpt
                    .and_then([this](DisplayModeId modeId)
                                      REQUIRES(mLock) { return mDisplayModes.get(modeId); })
                    .transform([](const DisplayModePtr& modePtr) {
                        return FrameRateMode{modePtr->getPeakFps(), ftl::as_non_null(modePtr)};
                    })
                    .or_else([this] {
                        ftl::FakeGuard guard(mLock);
                        return std::make_optional(getActiveModeLocked());
                    })
                    .value();

    const DisplayModePtr& min = mMinRefreshRateModeIt->second;
    if (current.modePtr->getId() == min->getId()) {
        return {};
    }

    return timerExpired ? FrameRateMode{min->getPeakFps(), ftl::as_non_null(min)} : current;
}

const DisplayModePtr& RefreshRateSelector::getMinRefreshRateByPolicyLocked() const {
    const auto& activeMode = *getActiveModeLocked().modePtr;

    for (const FrameRateMode& mode : mPrimaryFrameRates) {
        if (activeMode.getGroup() == mode.modePtr->getGroup()) {
            return mode.modePtr.get();
        }
    }

    ALOGE("Can't find min refresh rate by policy with the same mode group as the current mode %s",
          to_string(activeMode).c_str());

    // Default to the lowest refresh rate.
    return mPrimaryFrameRates.front().modePtr.get();
}

const DisplayModePtr& RefreshRateSelector::getMaxRefreshRateByPolicyLocked(int anchorGroup) const {
    const ftl::NonNull<DisplayModePtr>* maxByAnchor = &mPrimaryFrameRates.back().modePtr;
    const ftl::NonNull<DisplayModePtr>* max = &mPrimaryFrameRates.back().modePtr;

    bool maxByAnchorFound = false;
    for (auto it = mPrimaryFrameRates.rbegin(); it != mPrimaryFrameRates.rend(); ++it) {
        using namespace fps_approx_ops;
        if (it->modePtr->getPeakFps() > (*max)->getPeakFps()) {
            max = &it->modePtr;
        }

        if (anchorGroup == it->modePtr->getGroup() &&
            it->modePtr->getPeakFps() >= (*maxByAnchor)->getPeakFps()) {
            maxByAnchorFound = true;
            maxByAnchor = &it->modePtr;
        }
    }

    if (maxByAnchorFound) {
        return maxByAnchor->get();
    }

    ALOGE("Can't find max refresh rate by policy with the same group %d", anchorGroup);

    // Default to the highest refresh rate.
    return max->get();
}

auto RefreshRateSelector::rankFrameRates(std::optional<int> anchorGroupOpt,
                                         RefreshRateOrder refreshRateOrder,
                                         std::optional<DisplayModeId> preferredDisplayModeOpt,
                                         const RankFrameRatesPredicate& predicate) const
        -> FrameRateRanking {
    using fps_approx_ops::operator<;
    const char* const whence = __func__;

    // find the highest frame rate for each display mode
    ftl::SmallMap<DisplayModeId, Fps, 8> maxRenderRateForMode;
    const bool ascending = (refreshRateOrder == RefreshRateOrder::Ascending);
    if (ascending) {
        // TODO(b/266481656): Once this bug is fixed, we can remove this workaround and actually
        //  use a lower frame rate when we want Ascending frame rates.
        for (const auto& frameRateMode : mPrimaryFrameRates) {
            if (anchorGroupOpt && frameRateMode.modePtr->getGroup() != anchorGroupOpt) {
                continue;
            }

            const auto [iter, _] = maxRenderRateForMode.try_emplace(frameRateMode.modePtr->getId(),
                                                                    frameRateMode.fps);
            if (iter->second < frameRateMode.fps) {
                iter->second = frameRateMode.fps;
            }
        }
    }

    std::deque<ScoredFrameRate> ranking;
    const auto rankFrameRate = [&](const FrameRateMode& frameRateMode) REQUIRES(mLock) {
        const auto& modePtr = frameRateMode.modePtr;
        if ((anchorGroupOpt && modePtr->getGroup() != anchorGroupOpt) ||
            !predicate(frameRateMode)) {
            return;
        }

        const bool ascending = (refreshRateOrder == RefreshRateOrder::Ascending);
        const auto id = modePtr->getId();
        if (ascending && frameRateMode.fps < *maxRenderRateForMode.get(id)) {
            // TODO(b/266481656): Once this bug is fixed, we can remove this workaround and actually
            //  use a lower frame rate when we want Ascending frame rates.
            return;
        }

        float score = calculateDistanceScoreFromMaxLocked(frameRateMode.fps);

        if (ascending) {
            score = 1.0f / score;
        }

        constexpr float kScore = std::numeric_limits<float>::max();
        if (preferredDisplayModeOpt) {
            if (*preferredDisplayModeOpt == modePtr->getId()) {
                ranking.emplace_front(ScoredFrameRate{frameRateMode, kScore});
                return;
            }
            constexpr float kNonPreferredModePenalty = 0.95f;
            score *= kNonPreferredModePenalty;
        } else if (ascending && id == getMinRefreshRateByPolicyLocked()->getId()) {
            // TODO(b/266481656): Once this bug is fixed, we can remove this workaround
            //  and actually use a lower frame rate when we want Ascending frame rates.
            ranking.emplace_front(ScoredFrameRate{frameRateMode, kScore});
            return;
        }

        ALOGV("%s(%s) %s (%s(%s)) scored %.2f", whence, ftl::enum_string(refreshRateOrder).c_str(),
              to_string(frameRateMode.fps).c_str(), to_string(modePtr->getPeakFps()).c_str(),
              to_string(modePtr->getVsyncRate()).c_str(), score);
        ranking.emplace_back(ScoredFrameRate{frameRateMode, score});
    };

    if (refreshRateOrder == RefreshRateOrder::Ascending) {
        std::for_each(mPrimaryFrameRates.begin(), mPrimaryFrameRates.end(), rankFrameRate);
    } else {
        std::for_each(mPrimaryFrameRates.rbegin(), mPrimaryFrameRates.rend(), rankFrameRate);
    }

    if (!ranking.empty() || !anchorGroupOpt) {
        return {ranking.begin(), ranking.end()};
    }

    ALOGW("Can't find %s refresh rate by policy with the same mode group"
          " as the mode group %d",
          refreshRateOrder == RefreshRateOrder::Ascending ? "min" : "max", anchorGroupOpt.value());

    constexpr std::optional<int> kNoAnchorGroup = std::nullopt;
    return rankFrameRates(kNoAnchorGroup, refreshRateOrder, preferredDisplayModeOpt);
}

FrameRateMode RefreshRateSelector::getActiveMode() const {
    std::lock_guard lock(mLock);
    return getActiveModeLocked();
}

const FrameRateMode& RefreshRateSelector::getActiveModeLocked() const {
    return *mActiveModeOpt;
}

void RefreshRateSelector::setActiveMode(DisplayModeId modeId, Fps renderFrameRate) {
    std::lock_guard lock(mLock);

    // Invalidate the cached invocation to getRankedFrameRates. This forces
    // the refresh rate to be recomputed on the next call to getRankedFrameRates.
    mGetRankedFrameRatesCache.reset();

    const auto activeModeOpt = mDisplayModes.get(modeId);
    LOG_ALWAYS_FATAL_IF(!activeModeOpt);

    mActiveModeOpt.emplace(FrameRateMode{renderFrameRate, ftl::as_non_null(activeModeOpt->get())});
    mIsVrrDevice = FlagManager::getInstance().vrr_config() &&
            activeModeOpt->get()->getVrrConfig().has_value();
}

RefreshRateSelector::RefreshRateSelector(DisplayModes modes, DisplayModeId activeModeId,
                                         Config config)
      : mKnownFrameRates(constructKnownFrameRates(modes)), mConfig(config) {
    initializeIdleTimer(mConfig.legacyIdleTimerTimeout);
    FTL_FAKE_GUARD(kMainThreadContext, updateDisplayModes(std::move(modes), activeModeId));
}

void RefreshRateSelector::initializeIdleTimer(std::chrono::milliseconds timeout) {
    if (timeout > 0ms) {
        mIdleTimer.emplace(
                "IdleTimer", timeout,
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onReset();
                    }
                },
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onExpired();
                    }
                });
    }
}

void RefreshRateSelector::updateDisplayModes(DisplayModes modes, DisplayModeId activeModeId) {
    std::lock_guard lock(mLock);

    // Invalidate the cached invocation to getRankedFrameRates. This forces
    // the refresh rate to be recomputed on the next call to getRankedFrameRates.
    mGetRankedFrameRatesCache.reset();

    mDisplayModes = std::move(modes);
    const auto activeModeOpt = mDisplayModes.get(activeModeId);
    LOG_ALWAYS_FATAL_IF(!activeModeOpt);
    mActiveModeOpt = FrameRateMode{activeModeOpt->get()->getPeakFps(),
                                   ftl::as_non_null(activeModeOpt->get())};

    const auto sortedModes = sortByRefreshRate(mDisplayModes);
    mMinRefreshRateModeIt = sortedModes.front();
    mMaxRefreshRateModeIt = sortedModes.back();

    // Reset the policy because the old one may no longer be valid.
    mDisplayManagerPolicy = {};
    mDisplayManagerPolicy.defaultMode = activeModeId;

    mFrameRateOverrideConfig = [&] {
        switch (mConfig.enableFrameRateOverride) {
            case Config::FrameRateOverride::Disabled:
            case Config::FrameRateOverride::AppOverride:
            case Config::FrameRateOverride::Enabled:
                return mConfig.enableFrameRateOverride;
            case Config::FrameRateOverride::AppOverrideNativeRefreshRates:
                return shouldEnableFrameRateOverride(sortedModes)
                        ? Config::FrameRateOverride::AppOverrideNativeRefreshRates
                        : Config::FrameRateOverride::Disabled;
        }
    }();

    if (mConfig.enableFrameRateOverride ==
        Config::FrameRateOverride::AppOverrideNativeRefreshRates) {
        for (const auto& [_, mode] : mDisplayModes) {
            mAppOverrideNativeRefreshRates.try_emplace(mode->getPeakFps(), ftl::unit);
        }
    }

    constructAvailableRefreshRates();
}

bool RefreshRateSelector::isPolicyValidLocked(const Policy& policy) const {
    // defaultMode must be a valid mode, and within the given refresh rate range.
    if (const auto mode = mDisplayModes.get(policy.defaultMode)) {
        if (!policy.primaryRanges.physical.includes(mode->get()->getPeakFps())) {
            ALOGE("Default mode is not in the primary range.");
            return false;
        }
    } else {
        ALOGE("Default mode is not found.");
        return false;
    }

    const auto& primaryRanges = policy.primaryRanges;
    const auto& appRequestRanges = policy.appRequestRanges;
    ALOGE_IF(!appRequestRanges.physical.includes(primaryRanges.physical),
             "Physical range is invalid: primary: %s appRequest: %s",
             to_string(primaryRanges.physical).c_str(),
             to_string(appRequestRanges.physical).c_str());
    ALOGE_IF(!appRequestRanges.render.includes(primaryRanges.render),
             "Render range is invalid: primary: %s appRequest: %s",
             to_string(primaryRanges.render).c_str(), to_string(appRequestRanges.render).c_str());

    return primaryRanges.valid() && appRequestRanges.valid();
}

auto RefreshRateSelector::setPolicy(const PolicyVariant& policy) -> SetPolicyResult {
    Policy oldPolicy;
    PhysicalDisplayId displayId;
    {
        std::lock_guard lock(mLock);
        oldPolicy = *getCurrentPolicyLocked();

        const bool valid = ftl::match(
                policy,
                [this](const auto& policy) {
                    ftl::FakeGuard guard(mLock);
                    if (!isPolicyValidLocked(policy)) {
                        ALOGE("Invalid policy: %s", policy.toString().c_str());
                        return false;
                    }

                    using T = std::decay_t<decltype(policy)>;

                    if constexpr (std::is_same_v<T, DisplayManagerPolicy>) {
                        mDisplayManagerPolicy = policy;
                    } else {
                        static_assert(std::is_same_v<T, OverridePolicy>);
                        mOverridePolicy = policy;
                    }
                    return true;
                },
                [this](NoOverridePolicy) {
                    ftl::FakeGuard guard(mLock);
                    mOverridePolicy.reset();
                    return true;
                });

        if (!valid) {
            return SetPolicyResult::Invalid;
        }

        mGetRankedFrameRatesCache.reset();

        const auto& idleScreenConfigOpt = getCurrentPolicyLocked()->idleScreenConfigOpt;
        if (idleScreenConfigOpt != oldPolicy.idleScreenConfigOpt) {
            if (!idleScreenConfigOpt.has_value()) {
                if (mIdleTimer) {
                    // fallback to legacy timer if existed, otherwise pause the old timer
                    if (mConfig.legacyIdleTimerTimeout > 0ms) {
                        mIdleTimer->setInterval(mConfig.legacyIdleTimerTimeout);
                        mIdleTimer->resume();
                    } else {
                        mIdleTimer->pause();
                    }
                }
            } else if (idleScreenConfigOpt->timeoutMillis > 0) {
                // create a new timer or reconfigure
                const auto timeout = std::chrono::milliseconds{idleScreenConfigOpt->timeoutMillis};
                if (!mIdleTimer) {
                    initializeIdleTimer(timeout);
                    if (mIdleTimerStarted) {
                        mIdleTimer->start();
                    }
                } else {
                    mIdleTimer->setInterval(timeout);
                    mIdleTimer->resume();
                }
            } else {
                if (mIdleTimer) {
                    mIdleTimer->pause();
                }
            }
        }

        if (getCurrentPolicyLocked()->similarExceptIdleConfig(oldPolicy)) {
            return SetPolicyResult::Unchanged;
        }

        constructAvailableRefreshRates();

        displayId = getActiveModeLocked().modePtr->getPhysicalDisplayId();
    }

    const unsigned numModeChanges = std::exchange(mNumModeSwitchesInPolicy, 0u);

    ALOGI("Display %s policy changed\n"
          "Previous: %s\n"
          "Current:  %s\n"
          "%u mode changes were performed under the previous policy",
          to_string(displayId).c_str(), oldPolicy.toString().c_str(), toString(policy).c_str(),
          numModeChanges);

    return SetPolicyResult::Changed;
}

auto RefreshRateSelector::getCurrentPolicyLocked() const -> const Policy* {
    return mOverridePolicy ? &mOverridePolicy.value() : &mDisplayManagerPolicy;
}

auto RefreshRateSelector::getCurrentPolicy() const -> Policy {
    std::lock_guard lock(mLock);
    return *getCurrentPolicyLocked();
}

auto RefreshRateSelector::getDisplayManagerPolicy() const -> Policy {
    std::lock_guard lock(mLock);
    return mDisplayManagerPolicy;
}

bool RefreshRateSelector::isModeAllowed(const FrameRateMode& mode) const {
    std::lock_guard lock(mLock);
    return std::find(mAppRequestFrameRates.begin(), mAppRequestFrameRates.end(), mode) !=
            mAppRequestFrameRates.end();
}

void RefreshRateSelector::constructAvailableRefreshRates() {
    // Filter modes based on current policy and sort on refresh rate.
    const Policy* policy = getCurrentPolicyLocked();
    ALOGV("%s: %s ", __func__, policy->toString().c_str());

    const auto& defaultMode = mDisplayModes.get(policy->defaultMode)->get();

    const auto filterRefreshRates = [&](const FpsRanges& ranges,
                                        const char* rangeName) REQUIRES(mLock) {
        const auto filterModes = [&](const DisplayMode& mode) {
            return mode.getResolution() == defaultMode->getResolution() &&
                    mode.getDpi() == defaultMode->getDpi() &&
                    (policy->allowGroupSwitching || mode.getGroup() == defaultMode->getGroup()) &&
                    ranges.physical.includes(mode.getPeakFps()) &&
                    (supportsFrameRateOverride() || ranges.render.includes(mode.getPeakFps()));
        };

        auto frameRateModes = createFrameRateModes(*policy, filterModes, ranges.render);
        if (frameRateModes.empty()) {
            ALOGW("No matching frame rate modes for %s range. policy: %s", rangeName,
                  policy->toString().c_str());
            // TODO(b/292105422): Ideally DisplayManager should not send render ranges smaller than
            // the min supported. See b/292047939.
            //  For not we just ignore the render ranges.
            frameRateModes = createFrameRateModes(*policy, filterModes, {});
        }
        LOG_ALWAYS_FATAL_IF(frameRateModes.empty(),
                            "No matching frame rate modes for %s range even after ignoring the "
                            "render range. policy: %s",
                            rangeName, policy->toString().c_str());

        const auto stringifyModes = [&] {
            std::string str;
            for (const auto& frameRateMode : frameRateModes) {
                str += to_string(frameRateMode) + " ";
            }
            return str;
        };
        ALOGV("%s render rates: %s, isVrrDevice? %d", rangeName, stringifyModes().c_str(),
              mIsVrrDevice.load());

        return frameRateModes;
    };

    mPrimaryFrameRates = filterRefreshRates(policy->primaryRanges, "primary");
    mAppRequestFrameRates = filterRefreshRates(policy->appRequestRanges, "app request");
    // Idle refresh rate is 60 Hz while respecting minimum of primary and app ranges.
    const Fps minPrimary = mPrimaryFrameRates.front().modePtr.get()->getVsyncRate();
    const Fps minAppRequest = mAppRequestFrameRates.front().modePtr.get()->getVsyncRate();
    mIdleFps = std::max({60_Hz, minPrimary, minAppRequest}, fps_approx_ops::operator<);

    ALOGV("idle refresh rate calculated: %s", to_string(mIdleFps).c_str());

    mAllFrameRates = filterRefreshRates(FpsRanges(getSupportedFrameRateRangeLocked(),
                                                  getSupportedFrameRateRangeLocked()),
                                        "full frame rates");
}

bool RefreshRateSelector::isVrrDevice() const {
    return false;
}

Fps RefreshRateSelector::findClosestKnownFrameRate(Fps frameRate) const {
    using namespace fps_approx_ops;

    if (frameRate <= mKnownFrameRates.front()) {
        return mKnownFrameRates.front();
    }

    if (frameRate >= mKnownFrameRates.back()) {
        return mKnownFrameRates.back();
    }

    auto lowerBound = std::lower_bound(mKnownFrameRates.begin(), mKnownFrameRates.end(), frameRate,
                                       isStrictlyLess);

    const auto distance1 = std::abs(frameRate.getValue() - lowerBound->getValue());
    const auto distance2 = std::abs(frameRate.getValue() - std::prev(lowerBound)->getValue());
    return distance1 < distance2 ? *lowerBound : *std::prev(lowerBound);
}

std::vector<float> RefreshRateSelector::getSupportedFrameRates() const {
    std::scoped_lock lock(mLock);
    // TODO(b/356986687) Remove the limit once we have the anchor list implementation.
    const size_t frameRatesSize = std::min<size_t>(11, mAllFrameRates.size());
    std::vector<float> supportedFrameRates;
    supportedFrameRates.reserve(frameRatesSize);
    std::transform(mAllFrameRates.rbegin(),
                   mAllFrameRates.rbegin() + static_cast<int>(frameRatesSize),
                   std::back_inserter(supportedFrameRates),
                   [](FrameRateMode mode) { return mode.fps.getValue(); });
    return supportedFrameRates;
}

FpsRange RefreshRateSelector::getSupportedFrameRateRangeLocked() const {
    using fps_approx_ops::operator<;
    if (mMaxRefreshRateModeIt->second->getPeakFps() < kMinSupportedFrameRate) {
        return {mMaxRefreshRateModeIt->second->getPeakFps(), kMinSupportedFrameRate};
    }
    return {kMinSupportedFrameRate, mMaxRefreshRateModeIt->second->getPeakFps()};
}

auto RefreshRateSelector::getIdleTimerAction() const -> KernelIdleTimerAction {
    return KernelIdleTimerAction::TurnOff;
}

int RefreshRateSelector::getFrameRateDivisor(Fps displayRefreshRate, Fps layerFrameRate) {
    // This calculation needs to be in sync with the java code
    // in DisplayManagerService.getDisplayInfoForFrameRateOverride

    // The threshold must be smaller than 0.001 in order to differentiate
    // between the fractional pairs (e.g. 59.94 and 60).
    constexpr float kThreshold = 0.0009f;
    const auto numPeriods = displayRefreshRate.getValue() / layerFrameRate.getValue();
    const auto numPeriodsRounded = std::round(numPeriods);
    if (std::abs(numPeriods - numPeriodsRounded) > kThreshold) {
        return 0;
    }

    return static_cast<int>(numPeriodsRounded);
}

bool RefreshRateSelector::isFractionalPairOrMultiple(Fps smaller, Fps bigger) {
    if (isStrictlyLess(bigger, smaller)) {
        return isFractionalPairOrMultiple(bigger, smaller);
    }

    const auto multiplier = std::round(bigger.getValue() / smaller.getValue());
    constexpr float kCoef = 1000.f / 1001.f;
    return isApproxEqual(bigger, Fps::fromValue(smaller.getValue() * multiplier / kCoef)) ||
            isApproxEqual(bigger, Fps::fromValue(smaller.getValue() * multiplier * kCoef));
}

void RefreshRateSelector::dump(utils::Dumper& dumper) const {
    using namespace std::string_view_literals;

    std::lock_guard lock(mLock);

    const auto activeMode = getActiveModeLocked();
    dumper.dump("renderRate"sv, to_string(activeMode.fps));
    dumper.dump("activeMode"sv, to_string(*activeMode.modePtr));

    dumper.dump("displayModes"sv);
    {
        utils::Dumper::Indent indent(dumper);
        for (const auto& [id, mode] : mDisplayModes) {
            dumper.dump({}, to_string(*mode));
        }
    }

    dumper.dump("displayManagerPolicy"sv, mDisplayManagerPolicy.toString());

    if (const Policy& currentPolicy = *getCurrentPolicyLocked();
        mOverridePolicy && currentPolicy != mDisplayManagerPolicy) {
        dumper.dump("overridePolicy"sv, currentPolicy.toString());
    }

    dumper.dump("frameRateOverrideConfig"sv, *ftl::enum_name(mFrameRateOverrideConfig));

    dumper.dump("idleTimer"sv);
    {
        utils::Dumper::Indent indent(dumper);
        dumper.dump("interval"sv, mIdleTimer.transform(&OneShotTimer::interval));
        dumper.dump("controller"sv,
                    mConfig.kernelIdleTimerController
                            .and_then(&ftl::enum_name<KernelIdleTimerController>)
                            .value_or("Platform"sv));
    }
}

std::chrono::milliseconds RefreshRateSelector::getIdleTimerTimeout() {
    if (FlagManager::getInstance().idle_screen_refresh_rate_timeout() && mIdleTimer) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(mIdleTimer->interval());
    }
    return mConfig.legacyIdleTimerTimeout;
}

FpsRange RefreshRateSelector::getFrameRateCategoryRange(FrameRateCategory category) {
    (void)category;
    return FpsRange{60_Hz, 60_Hz};
}

} // namespace android::scheduler

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wextra"
