//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

/**
 * @fileoverview Manages theme (light/dark mode) toggling and persistence.
 */

import {LocalStorageManager} from './local_storage_manager';

/**
 * Supported color themes for the ExecuteQuery web UI.
 */
export type Theme = 'dark' | 'light';

/**
 * Controller managing light/dark theme toggling and persistence.
 */
export class ThemeManager {
  constructor(
    private readonly toggleButton: HTMLElement,
    private readonly storageManager: LocalStorageManager,
  ) {}

  /**
   * Attaches click listener and applies saved or system theme preference.
   */
  init(): this {
    this.toggleButton.addEventListener('click', () => {
      this.toggleTheme();
    });

    const theme = this.getSavedTheme();
    this.applyTheme(theme);
    this.updateButtonText(theme);
    return this;
  }

  /**
   * Toggles dark mode state, persists preference, and updates button UI.
   */
  private toggleTheme(): void {
    const isDarkMode = document.documentElement.classList.toggle('dark-mode');
    const theme: Theme = isDarkMode ? 'dark' : 'light';
    this.storageManager.updateSetting('theme', theme);
    this.updateButtonText(theme);
  }

  /**
   * Reads saved theme or checks system preference.
   */
  private getSavedTheme(): Theme {
    const theme = this.storageManager.getSetting('theme');
    if (theme === 'dark' || theme === 'light') return theme;
    return window.matchMedia('(prefers-color-scheme: dark)').matches
      ? 'dark'
      : 'light';
  }

  /**
   * Applies the given theme class to the document element.
   */
  private applyTheme(theme: Theme): void {
    document.documentElement.classList.toggle('dark-mode', theme === 'dark');
  }

  /**
   * Updates button accessible label and tooltip.
   */
  private updateButtonText(theme: Theme): void {
    this.toggleButton.setAttribute(
      'title',
      theme === 'dark' ? 'Switch to Light Mode' : 'Switch to Dark Mode',
    );
  }
}
