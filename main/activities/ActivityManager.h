#pragma once
#include "activities/Activity.h"
#include <M5GFX.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <esp_log.h>

class ActivityManager {
public:
    static ActivityManager &instance() { static ActivityManager m; return m; }

    void push(Activity *act) {
        dismiss_overlay(false);
        if (!m_stack.empty()) m_stack.back()->on_hide();
        m_stack.push_back(act);
        changed();
        act->on_create();
        act->on_show();
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "push → depth=%zu", m_stack.size());
    }

    void pop() {
        if (m_overlay) { dismiss_overlay(true); return; }
        if (m_stack.empty()) return;
        auto *old = m_stack.back(); m_stack.pop_back();
        changed();
        delete old;
        if (!m_stack.empty()) {
            m_stack.back()->on_show();
        }
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "pop → depth=%zu", m_stack.size());
    }

    void set(Activity *act) {
        dismiss_overlay(false);
        while (!m_stack.empty()) { delete m_stack.back(); m_stack.pop_back(); }
        push(act);
    }

    // Preserve the root activity; intermediate pages must not be shown while
    // clearing the stack (their destructors still release/save owned state).
    void pop_to_root() {
        dismiss_overlay(m_stack.size() <= 1);
        if (m_stack.size() <= 1) return;
        changed();
        while (m_stack.size() > 1) {
            auto *old = m_stack.back();
            m_stack.pop_back();
            delete old;
        }
        m_stack.front()->on_show();
        m_transition_cd = lgfx::millis() + 80;
        ESP_LOGI("AM", "pop to root → depth=%zu", m_stack.size());
    }


    bool pop_to(Activity *target) {
        auto found=std::find(m_stack.begin(),m_stack.end(),target);
        if(found==m_stack.end()) return false;
        dismiss_overlay(m_stack.back() == target);
        const size_t target_depth=size_t(found-m_stack.begin())+1;
        if(m_stack.size()==target_depth) return true;
        changed();
        while(m_stack.size()>target_depth) {
            auto *old=m_stack.back(); m_stack.pop_back(); delete old;
        }
        target->on_show();
        m_transition_cd=lgfx::millis()+80;
        return true;
    }
    // Replace the app branch without rendering its intermediate pages or root.
    void show_above_root(Activity *act) {
        dismiss_overlay(false);
        while(m_stack.size()>1) {
            auto *old=m_stack.back(); m_stack.pop_back(); delete old;
        }
        push(act);
    }

    // The underlying application keeps its workers, resources and lifecycle.
    bool show_overlay(Activity *act) {
        if (!act || m_overlay || m_stack.empty()) return false;
        m_overlay = act;
        changed();
        act->on_create();
        act->on_show();
        m_transition_cd = lgfx::millis() + 80;
        return true;
    }
    void dismiss_overlay(bool restore) {
        if (!m_overlay) return;
        auto *old = m_overlay;
        m_overlay = nullptr;
        changed();
        old->on_overlay_dismiss(restore);
        delete old;
        m_transition_cd = lgfx::millis() + 80;
    }
    bool has_overlay() const { return m_overlay != nullptr; }
    Activity *current() { return m_overlay ? m_overlay : m_stack.empty() ? nullptr : m_stack.back(); }
    bool is_blocked() { return lgfx::millis() < m_transition_cd; }
    size_t depth() const { return m_stack.size(); }
    bool can_pop() const { return m_overlay || m_stack.size() > 1; }
    uint32_t generation() const { return m_generation; }
    bool can_standby() const {
        if (m_overlay) return false;
        for (const auto* activity : m_stack) if (!activity->can_standby()) return false;
        return !m_stack.empty();
    }
    // Run application destructors (including reading saves), without showing
    // intermediate pages. Keep the root available if hardware power-off fails.
    void close_apps_for_shutdown() {
        dismiss_overlay(false);
        changed();
        while (m_stack.size() > 1) {
            auto* old = m_stack.back(); m_stack.pop_back(); delete old;
        }
    }

private:
    ActivityManager() = default;
    void changed() {
        ++m_generation;
#if CONFIG_IDF_TARGET_ESP32S31
        touch_input_set_context(m_generation);
#endif
    }
    std::vector<Activity *> m_stack;
    Activity *m_overlay = nullptr;
    uint32_t m_transition_cd = 0;
    uint32_t m_generation = 0;
};
