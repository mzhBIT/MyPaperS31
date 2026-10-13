#pragma once
#include "activities/Activity.h"
#include <M5GFX.h>
#include <cstdint>
#include <vector>
#include <esp_log.h>

class ActivityManager {
public:
    static ActivityManager &instance() { static ActivityManager m; return m; }

    void push(Activity *act) {
        if (!m_stack.empty()) m_stack.back()->on_hide();
        m_stack.push_back(act);
        act->on_create();
        act->on_show();
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "push → depth=%zu", m_stack.size());
    }

    void pop() {
        if (m_stack.empty()) return;
        auto *old = m_stack.back(); m_stack.pop_back();
        delete old;
        if (!m_stack.empty()) {
            m_stack.back()->on_show();
        }
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "pop → depth=%zu", m_stack.size());
    }

    void set(Activity *act) {
        while (!m_stack.empty()) { delete m_stack.back(); m_stack.pop_back(); }
        push(act);
    }

    // Preserve the root activity; intermediate pages must not be shown while
    // clearing the stack (their destructors still release/save owned state).
    void pop_to_root() {
        if (m_stack.size() <= 1) return;
        while (m_stack.size() > 1) {
            auto *old = m_stack.back();
            m_stack.pop_back();
            delete old;
        }
        m_stack.front()->on_show();
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "pop to root → depth=%zu", m_stack.size());
    }

    Activity *current() { return m_stack.empty() ? nullptr : m_stack.back(); }
    bool is_blocked() { return lgfx::millis() < m_transition_cd; }
    size_t depth() const { return m_stack.size(); }
    bool can_pop() const { return m_stack.size() > 1; }

private:
    ActivityManager() = default;
    std::vector<Activity *> m_stack;
    uint32_t m_transition_cd = 0;
};
