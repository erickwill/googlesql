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
 * @fileoverview Manages application settings stored in local persistent
 * storage.
 */

/**
 * Application settings stored in local persistent storage.
 */
export declare interface AppSettings {
  [key: string]: string | undefined;
  theme?: string;
  split_left_width?: string;
  layout_mode?: string;
}

/**
 * Manages the application settings stored in local persistent storage.
 */
export class LocalStorageManager {
  static readonly KEY = 'execute-query-settings';

  private getSettings(): AppSettings {
    try {
      const item = globalThis.localStorage?.getItem(LocalStorageManager.KEY);
      if (!item) return {};
      const parsed: unknown = JSON.parse(item);
      return parsed && typeof parsed === 'object'
        ? (parsed as AppSettings)
        : {};
    } catch {
      return {};
    }
  }

  getSetting<K extends keyof AppSettings>(key: K): AppSettings[K] {
    return this.getSettings()[key];
  }

  updateSetting<K extends keyof AppSettings>(
    key: K,
    value: AppSettings[K],
  ): void {
    const settings = this.getSettings();
    settings[key] = value;
    try {
      globalThis.localStorage?.setItem(
        LocalStorageManager.KEY,
        JSON.stringify(settings),
      );
    } catch {
      // Ignore storage quota or security errors in restricted browsing modes.
    }
  }
}
