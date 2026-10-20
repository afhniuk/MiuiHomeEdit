package io.github.afhniuk.miuihomeedit

import org.json.JSONObject

/**
 * The five switches stored in the JSON document.
 *
 * The keys are the contract with module_config.cpp: changing one here without
 * changing BitForKey() there silently disables that feature. All defaults are
 * false, so an absent file or a missing key never hides anything.
 */
data class LauncherConfig(
    val hideDrawerAllTab: Boolean = false,
    val hideEditButton: Boolean = false,
    val hideSettingsGear: Boolean = false,
    val hideSearchBox: Boolean = false,
    val hideSwipeUpArrow: Boolean = false,
) {
    fun toJson(): String = JSONObject().apply {
        put(KEY_VERSION, SCHEMA_VERSION)
        put(KEY_HIDE_DRAWER_ALL_TAB, hideDrawerAllTab)
        put(KEY_HIDE_EDIT_BUTTON, hideEditButton)
        put(KEY_HIDE_SETTINGS_GEAR, hideSettingsGear)
        put(KEY_HIDE_SEARCH_BOX, hideSearchBox)
        put(KEY_HIDE_SWIPE_UP_ARROW, hideSwipeUpArrow)
    }.toString(2)

    companion object {
        const val SCHEMA_VERSION = 1

        const val KEY_VERSION = "version"
        const val KEY_HIDE_DRAWER_ALL_TAB = "hide_drawer_all_tab"
        const val KEY_HIDE_EDIT_BUTTON = "hide_edit_button"
        const val KEY_HIDE_SETTINGS_GEAR = "hide_settings_gear"
        const val KEY_HIDE_SEARCH_BOX = "hide_search_box"
        const val KEY_HIDE_SWIPE_UP_ARROW = "hide_swipe_up_arrow"

        /** Throws JSONException for malformed input; ConfigStore catches it. */
        fun fromJson(text: String): LauncherConfig {
            val json = JSONObject(text)
            return LauncherConfig(
                hideDrawerAllTab = json.optBoolean(KEY_HIDE_DRAWER_ALL_TAB, false),
                hideEditButton = json.optBoolean(KEY_HIDE_EDIT_BUTTON, false),
                hideSettingsGear = json.optBoolean(KEY_HIDE_SETTINGS_GEAR, false),
                hideSearchBox = json.optBoolean(KEY_HIDE_SEARCH_BOX, false),
                hideSwipeUpArrow = json.optBoolean(KEY_HIDE_SWIPE_UP_ARROW, false),
            )
        }
    }
}
