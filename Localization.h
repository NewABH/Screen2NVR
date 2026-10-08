#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>

enum class UiLanguage : uint32_t
{
    russian = 0,
    english = 1,
};

inline uint32_t DefaultUiLanguage()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN
        ? static_cast<uint32_t>(UiLanguage::russian)
        : static_cast<uint32_t>(UiLanguage::english);
}

inline std::atomic<uint32_t> gUiLanguage{ DefaultUiLanguage() };

inline void SetUiLanguage(uint32_t language)
{
    gUiLanguage.store(language == static_cast<uint32_t>(UiLanguage::english) ? 1U : 0U,
                      std::memory_order_relaxed);
}

inline bool IsEnglishUi()
{
    return gUiLanguage.load(std::memory_order_relaxed) == static_cast<uint32_t>(UiLanguage::english);
}

inline const wchar_t* UiText(const wchar_t* russian, const wchar_t* english)
{
    return IsEnglishUi() ? english : russian;
}
