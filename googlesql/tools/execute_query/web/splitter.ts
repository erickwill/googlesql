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
 * @fileoverview Manages draggable panel splitting and layout mode toggling
 * (auto/vertical).
 */

import {LocalStorageManager} from './local_storage_manager';

/**
 * Layout modes supported by SplitterManager.
 */
export enum LayoutMode {
  AUTO = 'auto',
  VERTICAL = 'vertical',
}

const DEFAULT_LEFT_WIDTH_PCT = 40;

/**
 * Controller managing panel drag splitting and layout mode switching.
 */
export class SplitterManager {
  private isDragging = false;
  private currentMode: LayoutMode = LayoutMode.AUTO;

  constructor(
    private readonly splitter: HTMLElement,
    private readonly mainElement: HTMLElement,
    private readonly toggleButton: HTMLElement | null,
    private readonly storageManager: LocalStorageManager,
  ) {}

  /**
   * Attaches drag and layout toggle listeners and restores saved settings.
   */
  init(): this {
    this.splitter.addEventListener('mousedown', (e: Event) => {
      this.startDragging(e);
    });
    this.toggleButton?.addEventListener('click', () => {
      this.cycleLayoutMode();
    });

    // Restore saved width percentage and layout mode preference.
    const savedWidth = Number(
      this.storageManager.getSetting('split_left_width'),
    );
    if (!Number.isNaN(savedWidth) && savedWidth > 0) {
      this.setLeftWidthPercentage(Math.max(5, Math.min(95, savedWidth)));
    }

    const savedMode = this.storageManager.getSetting('layout_mode');
    if (savedMode === LayoutMode.AUTO || savedMode === LayoutMode.VERTICAL) {
      this.setLayoutMode(savedMode);
    } else {
      this.setLayoutMode(LayoutMode.AUTO);
    }
    return this;
  }

  /**
   * Attaches mouse drag listeners and applies resizing styling.
   */
  private startDragging(e: Event): void {
    e.preventDefault();
    this.isDragging = true;
    this.mainElement.classList.add('is-splitter-resizing');

    document.addEventListener('mousemove', this.onMouseMove);
    document.addEventListener('mouseup', this.stopDragging);
  }

  /**
   * Detaches mouse drag listeners and saves final width preference.
   */
  private readonly stopDragging = (): void => {
    if (!this.isDragging) return;
    this.isDragging = false;
    this.mainElement.classList.remove('is-splitter-resizing');

    document.removeEventListener('mousemove', this.onMouseMove);
    document.removeEventListener('mouseup', this.stopDragging);

    const currentPct = this.getLeftWidthPercentage();
    this.storageManager.updateSetting(
      'split_left_width',
      currentPct.toFixed(2),
    );
  };

  /**
   * Handles mouse drag motion to update split width percentage in the DOM.
   */
  private readonly onMouseMove = (e: MouseEvent): void => {
    if (!this.isDragging) return;
    const windowWidth = window.innerWidth;
    if (windowWidth <= 0) return;

    // Calculate mouse position percentage; pixel boundaries are enforced by CSS
    // Grid minmax().
    let pct = (e.clientX / windowWidth) * 100;
    pct = Math.max(5, Math.min(95, pct));
    this.setLeftWidthPercentage(pct);
  };

  /**
   * Updates the CSS custom property on the main element.
   */
  private setLeftWidthPercentage(pct: number): void {
    this.mainElement.style.setProperty('--left-width', `${pct}%`);
  }

  /**
   * Reads the currently applied left width percentage.
   */
  private getLeftWidthPercentage(): number {
    const val = this.mainElement.style.getPropertyValue('--left-width');
    const parsed = Number(val.replace('%', ''));
    return Number.isNaN(parsed) || parsed === 0
      ? DEFAULT_LEFT_WIDTH_PCT
      : parsed;
  }

  /**
   * Applies layout mode (auto or vertical).
   */
  private setLayoutMode(mode: LayoutMode): void {
    this.currentMode = mode;
    this.mainElement.classList.remove('layout-vertical');

    if (mode === LayoutMode.VERTICAL) {
      this.mainElement.classList.add('layout-vertical');
    }

    this.storageManager.updateSetting('layout_mode', mode);
  }

  /**
   * Gets the current layout mode.
   */
  getLayoutMode(): LayoutMode {
    return this.currentMode;
  }

  /**
   * Toggles between auto responsive and vertical stacked modes.
   */
  private cycleLayoutMode(): void {
    const nextMode =
      this.currentMode === LayoutMode.AUTO
        ? LayoutMode.VERTICAL
        : LayoutMode.AUTO;
    this.setLayoutMode(nextMode);
  }
}
