// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview 'os-settings-languages' handles Chrome's language and input
 * method settings. The 'languages' property, which reflects the current
 * language settings, must not be changed directly. Instead, changes to
 * language settings should be made using the LanguageHelper APIs provided by
 * this class via languageHelper.
 */

(function() {
'use strict';

cr.exportPath('settings');

const MoveType = chrome.languageSettingsPrivate.MoveType;

// Translate server treats some language codes the same.
// See also: components/translate/core/common/translate_util.cc.
const kLanguageCodeToTranslateCode = {
  'nb': 'no',
  'fil': 'tl',
  'zh-HK': 'zh-TW',
  'zh-MO': 'zh-TW',
  'zh-SG': 'zh-CN',
};

// Some ISO 639 language codes have been renamed, e.g. "he" to "iw", but
// Translate still uses the old versions. TODO(michaelpg): Chrome does too.
// Follow up with Translate owners to understand the right thing to do.
const kTranslateLanguageSynonyms = {
  'he': 'iw',
  'jv': 'jw',
};

// The fake language name used for ARC IMEs. The value must be in sync with the
// one in ui/base/ime/chromeos/extension_ime_util.h.
const kArcImeLanguage = '_arc_ime_language_';

const preferredLanguagesPrefName = 'settings.language.preferred_languages';

/**
 * Singleton element that generates the languages model on start-up and
 * updates it whenever Chrome's pref store and other settings change.
 * @implements {LanguageHelper}
 */
Polymer({
  is: 'os-settings-languages',

  behaviors: [PrefsBehavior],

  properties: {
    /**
     * @type {!LanguagesModel|undefined}
     */
    languages: {
      type: Object,
      notify: true,
      readOnly: true,
    },

    /**
     * This element, as a LanguageHelper instance for API usage.
     * @type {!LanguageHelper}
     */
    languageHelper: {
      type: Object,
      notify: true,
      readOnly: true,
      value: function() {
        return /** @type {!LanguageHelper} */ (this);
      },
    },

    /**
     * PromiseResolver to be resolved when the singleton has been initialized.
     * @private {!PromiseResolver}
     */
    resolver_: {
      type: Object,
      value: function() {
        return new PromiseResolver();
      },
    },

    /**
     * Hash map of supported languages by language codes for fast lookup.
     * @private {!Map<string, !chrome.languageSettingsPrivate.Language>}
     */
    supportedLanguageMap_: {
      type: Object,
      value: function() {
        return new Map();
      },
    },

    /**
     * Hash set of enabled language codes for membership testing.
     * @private {!Set<string>}
     */
    enabledLanguageSet_: {
      type: Object,
      value: function() {
        return new Set();
      },
    },

    /**
     * Hash map of supported input methods by ID for fast lookup.
     * @private {!Map<string, chrome.languageSettingsPrivate.InputMethod>}
     */
    supportedInputMethodMap_: {
      type: Object,
      value: function() {
        return new Map();
      },
    },

    /**
     * Hash map of input methods supported for each language.
     * @type {!Map<string,
     *             !Array<!chrome.languageSettingsPrivate.InputMethod>>}
     * @private
     */
    languageInputMethods_: {
      type: Object,
      value: function() {
        return new Map();
      },
    },

    /** @private Prospective UI language when the page was loaded. */
    originalProspectiveUILanguage_: String,
  },

  observers: [
    // All observers wait for the model to be populated by including the
    // |languages| property.
    'prospectiveUILanguageChanged_(prefs.intl.app_locale.value, languages)',
    'preferredLanguagesPrefChanged_(' +
        'prefs.' + preferredLanguagesPrefName + '.value, languages)',
    'translateLanguagesPrefChanged_(' +
        'prefs.translate_blocked_languages.value.*, languages)',
    'updateRemovableLanguages_(' +
        'prefs.intl.app_locale.value, languages.enabled)',
    'updateRemovableLanguages_(' +
        'prefs.translate_blocked_languages.value.*)',
    'updateRemovableLanguages_(' +
        'prefs.settings.language.preload_engines.value, ' +
        'prefs.settings.language.enabled_extension_imes.value, ' +
        'languages)',
  ],

  /** @private {?Function} */
  boundOnInputMethodChanged_: null,

  /** @private {?settings.LanguagesBrowserProxy} */
  browserProxy_: null,

  /** @private {?LanguageSettingsPrivate} */
  languageSettingsPrivate_: null,

  /** @private {?InputMethodPrivate} */
  inputMethodPrivate_: null,

  /** @private {?Function} */
  boundOnInputMethodAdded_: null,

  /** @private {?Function} */
  boundOnInputMethodRemoved_: null,

  /** @override */
  attached: function() {
    this.browserProxy_ = settings.LanguagesBrowserProxyImpl.getInstance();
    this.languageSettingsPrivate_ =
        this.browserProxy_.getLanguageSettingsPrivate();
    this.inputMethodPrivate_ = this.browserProxy_.getInputMethodPrivate();

    const promises = [];

    // Wait until prefs are initialized before creating the model, so we can
    // include information about enabled languages.
    promises[0] = CrSettingsPrefs.initialized;

    // Get the language list.
    promises[1] = new Promise(resolve => {
      this.languageSettingsPrivate_.getLanguageList(resolve);
    });

    // Get the translate target language.
    promises[2] = new Promise(resolve => {
      this.languageSettingsPrivate_.getTranslateTargetLanguage(resolve);
    });

    promises[3] = new Promise(resolve => {
      this.languageSettingsPrivate_.getInputMethodLists(function(lists) {
        resolve(
            lists.componentExtensionImes.concat(lists.thirdPartyExtensionImes));
      });
    });

    promises[4] = new Promise(resolve => {
      this.inputMethodPrivate_.getCurrentInputMethod(resolve);
    });

    // Fetch the starting UI language, which affects which actions should be
    // enabled.
    promises.push(this.browserProxy_.getProspectiveUILanguage().then(
        prospectiveUILanguage => {
          this.originalProspectiveUILanguage_ =
              prospectiveUILanguage || window.navigator.language;
        }));

    Promise.all(promises).then(results => {
      if (!this.isConnected) {
        // Return early if this element was detached from the DOM before
        // this async callback executes (can happen during testing).
        return;
      }

      this.createModel_(results[1], results[2], results[3], results[4]);
      this.resolver_.resolve();
    });

    this.boundOnInputMethodChanged_ = this.onInputMethodChanged_.bind(this);
    this.inputMethodPrivate_.onChanged.addListener(
        assert(this.boundOnInputMethodChanged_));
    this.boundOnInputMethodAdded_ = this.onInputMethodAdded_.bind(this);
    this.languageSettingsPrivate_.onInputMethodAdded.addListener(
        this.boundOnInputMethodAdded_);
    this.boundOnInputMethodRemoved_ = this.onInputMethodRemoved_.bind(this);
    this.languageSettingsPrivate_.onInputMethodRemoved.addListener(
        this.boundOnInputMethodRemoved_);
  },

  /** @override */
  detached: function() {
    this.inputMethodPrivate_.onChanged.removeListener(
        assert(this.boundOnInputMethodChanged_));
    this.boundOnInputMethodChanged_ = null;
    this.languageSettingsPrivate_.onInputMethodAdded.removeListener(
        assert(this.boundOnInputMethodAdded_));
    this.boundOnInputMethodAdded_ = null;
    this.languageSettingsPrivate_.onInputMethodRemoved.removeListener(
        assert(this.boundOnInputMethodRemoved_));
    this.boundOnInputMethodRemoved_ = null;
  },

  /**
   * Updates the prospective UI language based on the new pref value.
   * @param {string} prospectiveUILanguage
   * @private
   */
  prospectiveUILanguageChanged_: function(prospectiveUILanguage) {
    this.set(
        'languages.prospectiveUILanguage',
        prospectiveUILanguage || this.originalProspectiveUILanguage_);
  },

  /**
   * Updates the list of enabled languages from the preferred languages pref.
   * @private
   */
  preferredLanguagesPrefChanged_: function() {
    if (this.prefs == undefined || this.languages == undefined) {
      return;
    }

    const enabledLanguageStates = this.getEnabledLanguageStates_(
        this.languages.translateTarget, this.languages.prospectiveUILanguage);

    // Recreate the enabled language set before updating languages.enabled.
    this.enabledLanguageSet_.clear();
    for (let i = 0; i < enabledLanguageStates.length; i++) {
      this.enabledLanguageSet_.add(enabledLanguageStates[i].language.code);
    }

    this.set('languages.enabled', enabledLanguageStates);

    // Update translate target language.
    new Promise(resolve => {
      this.languageSettingsPrivate_.getTranslateTargetLanguage(resolve);
    }).then(result => {
      this.set('languages.translateTarget', result);
    });
  },

  /** @private */
  translateLanguagesPrefChanged_: function() {
    if (this.prefs == undefined || this.languages == undefined) {
      return;
    }

    const translateBlockedPref = this.getPref('translate_blocked_languages');
    const translateBlockedSet = this.makeSetFromArray_(
        /** @type {!Array<string>} */ (translateBlockedPref.value));

    for (let i = 0; i < this.languages.enabled.length; i++) {
      const language = this.languages.enabled[i].language;
      const translateEnabled = this.isTranslateEnabled_(
          language.code, !!language.supportsTranslate, translateBlockedSet,
          this.languages.translateTarget, this.languages.prospectiveUILanguage);
      this.set(
          'languages.enabled.' + i + '.translateEnabled', translateEnabled);
    }
  },

  /**
   * Constructs the languages model.
   * @param {!Array<!chrome.languageSettingsPrivate.Language>}
   *     supportedLanguages
   * @param {string} translateTarget Language code of the default translate
   *     target language.
   * @param {!Array<!chrome.languageSettingsPrivate.InputMethod>}
   *     supportedInputMethods Input methods.
   * @param {string} currentInputMethodId ID of the currently used
   *     input method.
   * @private
   */
  createModel_: function(
      supportedLanguages, translateTarget, supportedInputMethods,
      currentInputMethodId) {
    // Populate the hash map of supported languages.
    for (let i = 0; i < supportedLanguages.length; i++) {
      const language = supportedLanguages[i];
      language.supportsUI = !!language.supportsUI;
      language.supportsTranslate = !!language.supportsTranslate;
      language.isProhibitedLanguage = !!language.isProhibitedLanguage;
      this.supportedLanguageMap_.set(language.code, language);
    }

    if (supportedInputMethods) {
      this.createInputMethodModel_(supportedInputMethods);
    }

    const prospectiveUILanguage =
        /** @type {string} */ (this.getPref('intl.app_locale').value) ||
        this.originalProspectiveUILanguage_;

    // Create a list of enabled languages from the supported languages.
    const enabledLanguageStates =
        this.getEnabledLanguageStates_(translateTarget, prospectiveUILanguage);
    // Populate the hash set of enabled languages.
    for (let l = 0; l < enabledLanguageStates.length; l++) {
      this.enabledLanguageSet_.add(enabledLanguageStates[l].language.code);
    }

    const model = /** @type {!LanguagesModel} */ ({
      supported: supportedLanguages,
      enabled: enabledLanguageStates,
      translateTarget: translateTarget,
    });

    model.prospectiveUILanguage = prospectiveUILanguage;
    model.inputMethods = /** @type {!InputMethodsModel} */ ({
      supported: supportedInputMethods,
      enabled: this.getEnabledInputMethods_(),
      currentId: currentInputMethodId,
    });

    // Initialize the Polymer languages model.
    this._setLanguages(model);
  },

  /**
   * Constructs the input method part of the languages model.
   * @param {!Array<!chrome.languageSettingsPrivate.InputMethod>}
   *     supportedInputMethods Input methods.
   * @private
   */
  createInputMethodModel_: function(supportedInputMethods) {
    // Populate the hash map of supported input methods.
    this.supportedInputMethodMap_.clear();
    this.languageInputMethods_.clear();
    for (let j = 0; j < supportedInputMethods.length; j++) {
      const inputMethod = supportedInputMethods[j];
      inputMethod.enabled = !!inputMethod.enabled;
      inputMethod.isProhibitedByPolicy = !!inputMethod.isProhibitedByPolicy;
      // Add the input method to the map of IDs.
      this.supportedInputMethodMap_.set(inputMethod.id, inputMethod);
      // Add the input method to the list of input methods for each language
      // it supports.
      for (let k = 0; k < inputMethod.languageCodes.length; k++) {
        const languageCode = inputMethod.languageCodes[k];
        if (!this.supportedLanguageMap_.has(languageCode)) {
          continue;
        }
        if (!this.languageInputMethods_.has(languageCode)) {
          this.languageInputMethods_.set(languageCode, [inputMethod]);
        } else {
          this.languageInputMethods_.get(languageCode).push(inputMethod);
        }
      }
    }
  },

  /**
   * Returns a list of LanguageStates for each enabled language in the supported
   * languages list.
   * @param {string} translateTarget Language code of the default translate
   *     target language.
   * @param {(string|undefined)} prospectiveUILanguage Prospective UI display
   *     language.
   * @return {!Array<!LanguageState>}
   * @private
   */
  getEnabledLanguageStates_: function(translateTarget, prospectiveUILanguage) {
    assert(CrSettingsPrefs.isInitialized);

    const pref = this.getPref(preferredLanguagesPrefName);
    const enabledLanguageCodes = pref.value.split(',');

    const translateBlockedPref = this.getPref('translate_blocked_languages');
    const translateBlockedSet = this.makeSetFromArray_(
        /** @type {!Array<string>} */ (translateBlockedPref.value));

    const enabledLanguageStates = [];
    for (let i = 0; i < enabledLanguageCodes.length; i++) {
      const code = enabledLanguageCodes[i];
      const language = this.supportedLanguageMap_.get(code);
      // Skip unsupported languages.
      if (!language) {
        continue;
      }
      const languageState = /** @type {LanguageState} */ ({});
      languageState.language = language;
      languageState.translateEnabled = this.isTranslateEnabled_(
          code, !!language.supportsTranslate, translateBlockedSet,
          translateTarget, prospectiveUILanguage);
      enabledLanguageStates.push(languageState);
    }
    return enabledLanguageStates;
  },

  /**
   * True iff we translate pages that are in the given language.
   * @param {string} code Language code.
   * @param {boolean} supportsTranslate If translation supports the given
   *     language.
   * @param {!Set<string>} translateBlockedSet Set of languages for which
   *     translation is blocked.
   * @param {string} translateTarget Language code of the default translate
   *     target language.
   * @param {(string|undefined)} prospectiveUILanguage Prospective UI display
   *     language.
   * @return {boolean}
   * @private
   */
  isTranslateEnabled_: function(
      code, supportsTranslate, translateBlockedSet, translateTarget,
      prospectiveUILanguage) {
    const translateCode = this.convertLanguageCodeForTranslate(code);
    return supportsTranslate && !translateBlockedSet.has(translateCode) &&
        translateCode != translateTarget &&
        (!prospectiveUILanguage || code != prospectiveUILanguage);
  },

  /**
   * Returns a list of enabled input methods.
   * @return {!Array<!chrome.languageSettingsPrivate.InputMethod>}
   * @private
   */
  getEnabledInputMethods_: function() {
    assert(CrSettingsPrefs.isInitialized);

    let enabledInputMethodIds =
        this.getPref('settings.language.preload_engines').value.split(',');
    enabledInputMethodIds = enabledInputMethodIds.concat(
        this.getPref('settings.language.enabled_extension_imes')
            .value.split(','));

    // Return only supported input methods.
    return enabledInputMethodIds
        .map(id => this.supportedInputMethodMap_.get(id))
        .filter(function(inputMethod) {
          return !!inputMethod;
        });
  },

  /** @private */
  updateSupportedInputMethods_: function() {
    const promise = new Promise(resolve => {
      this.languageSettingsPrivate_.getInputMethodLists(function(lists) {
        resolve(
            lists.componentExtensionImes.concat(lists.thirdPartyExtensionImes));
      });
    });
    promise.then(result => {
      const supportedInputMethods = result;
      this.createInputMethodModel_(supportedInputMethods);
      this.set('languages.inputMethods.supported', supportedInputMethods);
      this.updateEnabledInputMethods_();
    });
  },

  /** @private */
  updateEnabledInputMethods_: function() {
    const enabledInputMethods = this.getEnabledInputMethods_();
    const enabledInputMethodSet = this.makeSetFromArray_(enabledInputMethods);

    for (let i = 0; i < this.languages.inputMethods.supported.length; i++) {
      this.set(
          'languages.inputMethods.supported.' + i + '.enabled',
          enabledInputMethodSet.has(this.languages.inputMethods.supported[i]));
    }
    this.set('languages.inputMethods.enabled', enabledInputMethods);
  },

  /**
   * Updates the |removable| property of the enabled language states based
   * on what other languages and input methods are enabled.
   * @private
   */
  updateRemovableLanguages_: function() {
    if (this.prefs == undefined || this.languages == undefined) {
      return;
    }

    // TODO(michaelpg): Enabled input methods can affect which languages are
    // removable, so run updateEnabledInputMethods_ first (if it has been
    // scheduled).
    this.updateEnabledInputMethods_();

    for (let i = 0; i < this.languages.enabled.length; i++) {
      const languageState = this.languages.enabled[i];
      this.set(
          'languages.enabled.' + i + '.removable',
          this.canDisableLanguage(languageState));
    }
  },

  /**
   * Creates a Set from the elements of the array.
   * @param {!Array<T>} list
   * @return {!Set<T>}
   * @template T
   * @private
   */
  makeSetFromArray_: function(list) {
    return new Set(list);
  },

  // LanguageHelper implementation.
  // TODO(michaelpg): replace duplicate docs with @override once b/24294625
  // is fixed.

  /** @return {!Promise} */
  whenReady: function() {
    return this.resolver_.promise;
  },

  /**
   * Sets the prospective UI language to the chosen language. This won't affect
   * the actual UI language until a restart.
   * @param {string} languageCode
   */
  setProspectiveUILanguage: function(languageCode) {
    this.browserProxy_.setProspectiveUILanguage(languageCode);
  },

  /**
   * True if the prospective UI language was changed from its starting value.
   * @return {boolean}
   */
  requiresRestart: function() {
    return this.originalProspectiveUILanguage_ !=
        this.languages.prospectiveUILanguage;
  },

  /**
   * @return {string} The language code for ARC IMEs.
   */
  getArcImeLanguageCode: function() {
    return kArcImeLanguage;
  },

  /**
   * @param {string} languageCode
   * @return {boolean} True if the language is for ARC IMEs.
   */
  isLanguageCodeForArcIme: function(languageCode) {
    return languageCode == kArcImeLanguage;
  },

  /**
   * @param {string} languageCode
   * @return {boolean} True if the language is enabled.
   */
  isLanguageEnabled: function(languageCode) {
    return this.enabledLanguageSet_.has(languageCode);
  },

  /**
   * Enables the language, making it available for input.
   * @param {string} languageCode
   */
  enableLanguage: function(languageCode) {
    if (!CrSettingsPrefs.isInitialized) {
      return;
    }

    this.languageSettingsPrivate_.enableLanguage(languageCode);
  },

  /**
   * Disables the language.
   * @param {string} languageCode
   */
  disableLanguage: function(languageCode) {
    if (!CrSettingsPrefs.isInitialized) {
      return;
    }

    // Remove input methods that don't support any other enabled language.
    const inputMethods = this.languageInputMethods_.get(languageCode) || [];
    for (let i = 0; i < inputMethods.length; i++) {
      const inputMethod = inputMethods[i];
      const supportsOtherEnabledLanguages = inputMethod.languageCodes.some(
          otherLanguageCode => otherLanguageCode != languageCode &&
              this.isLanguageEnabled(otherLanguageCode));
      if (!supportsOtherEnabledLanguages) {
        this.removeInputMethod(inputMethod.id);
      }
    }

    // Remove the language from preferred languages.
    this.languageSettingsPrivate_.disableLanguage(languageCode);
  },

  /**
   * @param {!LanguageState} languageState
   * @return {boolean}
   */
  isOnlyTranslateBlockedLanguage: function(languageState) {
    return !languageState.translateEnabled &&
        this.languages.enabled.filter(lang => !lang.translateEnabled).length ==
        1;
  },

  /**
   * @param {!LanguageState} languageState
   * @return {boolean}
   */
  canDisableLanguage: function(languageState) {
    // Cannot disable the prospective UI language.
    if (languageState.language.code == this.languages.prospectiveUILanguage) {
      return false;
    }

    // Cannot disable the only enabled language.
    if (this.languages.enabled.length == 1) {
      return false;
    }

    // Cannot disable the last translate blocked language.
    if (this.isOnlyTranslateBlockedLanguage(languageState)) {
      return false;
    }

    // If this is the only enabled language that is supported by all enabled
    // component IMEs, it cannot be disabled because we need those IMEs.
    const otherInputMethodsEnabled =
        this.languages.enabled.some(function(otherLanguageState) {
          const otherLanguageCode = otherLanguageState.language.code;
          if (otherLanguageCode == languageState.language.code) {
            return false;
          }
          const inputMethods =
              this.languageInputMethods_.get(otherLanguageCode);
          return inputMethods && inputMethods.some(function(inputMethod) {
            return this.isComponentIme(inputMethod) &&
                this.supportedInputMethodMap_.get(inputMethod.id).enabled;
          }, this);
        }, this);
    return otherInputMethodsEnabled;
  },

  /**
   * @param {!chrome.languageSettingsPrivate.Language} language
   * @return {boolean} true if the given language can be enabled
   */
  canEnableLanguage(language) {
    return !(
        this.isLanguageEnabled(language.code) ||
        language.isProhibitedLanguage ||
        this.isLanguageCodeForArcIme(language.code) /* internal use only */);
  },

  /**
   * Moves the language in the list of enabled languages either up (toward the
   * front of the list) or down (toward the back).
   * @param {string} languageCode
   * @param {boolean} upDirection True if we need to move up, false if we
   *     need to move down
   */
  moveLanguage: function(languageCode, upDirection) {
    if (!CrSettingsPrefs.isInitialized) {
      return;
    }

    if (upDirection) {
      this.languageSettingsPrivate_.moveLanguage(languageCode, MoveType.UP);
    } else {
      this.languageSettingsPrivate_.moveLanguage(languageCode, MoveType.DOWN);
    }
  },

  /**
   * Moves the language directly to the front of the list of enabled languages.
   * @param {string} languageCode
   */
  moveLanguageToFront: function(languageCode) {
    if (!CrSettingsPrefs.isInitialized) {
      return;
    }

    this.languageSettingsPrivate_.moveLanguage(languageCode, MoveType.TOP);
  },

  /**
   * Enables translate for the given language by removing the translate
   * language from the blocked languages preference.
   * @param {string} languageCode
   */
  enableTranslateLanguage: function(languageCode) {
    this.languageSettingsPrivate_.setEnableTranslationForLanguage(
        languageCode, true);
  },

  /**
   * Disables translate for the given language by adding the translate
   * language to the blocked languages preference.
   * @param {string} languageCode
   */
  disableTranslateLanguage: function(languageCode) {
    this.languageSettingsPrivate_.setEnableTranslationForLanguage(
        languageCode, false);
  },

  /**
   * Converts the language code for translate. There are some differences
   * between the language set the Translate server uses and that for
   * Accept-Language.
   * @param {string} languageCode
   * @return {string} The converted language code.
   */
  convertLanguageCodeForTranslate: function(languageCode) {
    if (languageCode in kLanguageCodeToTranslateCode) {
      return kLanguageCodeToTranslateCode[languageCode];
    }

    const main = languageCode.split('-')[0];
    if (main == 'zh') {
      // In Translate, general Chinese is not used, and the sub code is
      // necessary as a language code for the Translate server.
      return languageCode;
    }
    if (main in kTranslateLanguageSynonyms) {
      return kTranslateLanguageSynonyms[main];
    }

    return main;
  },

  /**
   * Given a language code, returns just the base language. E.g., converts
   * 'en-GB' to 'en'.
   * @param {string} languageCode
   * @return {string}
   */
  getLanguageCodeWithoutRegion: function(languageCode) {
    // The Norwegian languages fall under the 'no' macrolanguage.
    if (languageCode == 'nb' || languageCode == 'nn') {
      return 'no';
    }

    // Match the characters before the hyphen.
    const result = languageCode.match(/^([^-]+)-?/);
    assert(result.length == 2);
    return result[1];
  },

  /**
   * @param {string} languageCode
   * @return {!chrome.languageSettingsPrivate.Language|undefined}
   */
  getLanguage: function(languageCode) {
    // If a languageCode is not found, try language without location.
    return this.supportedLanguageMap_.get(languageCode) ||
        this.supportedLanguageMap_.get(
            this.getLanguageCodeWithoutRegion(languageCode));
  },

  /** @param {string} id */
  addInputMethod: function(id) {
    if (!this.supportedInputMethodMap_.has(id)) {
      return;
    }
    this.languageSettingsPrivate_.addInputMethod(id);
  },

  /** @param {string} id */
  removeInputMethod: function(id) {
    if (!this.supportedInputMethodMap_.has(id)) {
      return;
    }
    this.languageSettingsPrivate_.removeInputMethod(id);
  },

  /** @param {string} id */
  setCurrentInputMethod: function(id) {
    this.inputMethodPrivate_.setCurrentInputMethod(id);
  },

  /**
   * @param {string} languageCode
   * @return {!Array<!chrome.languageSettingsPrivate.InputMethod>}
   */
  getInputMethodsForLanguage: function(languageCode) {
    return this.languageInputMethods_.get(languageCode) || [];
  },

  /**
   * @param {!chrome.languageSettingsPrivate.InputMethod} inputMethod
   * @return {boolean}
   */
  isComponentIme: function(inputMethod) {
    return inputMethod.id.startsWith('_comp_');
  },

  /** @param {string} id Input method ID. */
  openInputMethodOptions: function(id) {
    this.inputMethodPrivate_.openOptionsPage(id);
  },

  /** @param {string} id New current input method ID. */
  onInputMethodChanged_: function(id) {
    this.set('languages.inputMethods.currentId', id);
  },

  /** @param {string} id Added input method ID. */
  onInputMethodAdded_: function(id) {
    this.updateSupportedInputMethods_();
  },

  /** @param {string} id Removed input method ID. */
  onInputMethodRemoved_: function(id) {
    this.updateSupportedInputMethods_();
  },
});
})();
