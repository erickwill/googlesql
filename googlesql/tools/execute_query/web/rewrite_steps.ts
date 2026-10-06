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
 * @fileoverview Manages interactive rewrite-step switching and diff rendering
 * in the analyze view.
 */

import {
  diffTextLines,
  renderDiffNodes,
  renderSideBySideDiffNodes,
  splitNodeLines,
} from './tree_diff';

/**
 * Display mode for rewrite step Resolved AST diffs.
 */
export type DiffMode = 'none' | 'inline' | 'side-by-side';

const DIFF_MODE_STORAGE_KEY = 'googlesql-diff-mode';
const DEFAULT_DIFF_MODE: DiffMode = 'inline';

function isDiffMode(value: string | null): value is DiffMode {
  return value === 'none' || value === 'inline' || value === 'side-by-side';
}

/**
 * Retrieves the saved diff mode from session storage (defaulting to 'inline').
 */
function getSavedDiffMode(): DiffMode {
  try {
    const savedMode = window.sessionStorage.getItem(DIFF_MODE_STORAGE_KEY);
    if (isDiffMode(savedMode)) {
      return savedMode;
    }
  } catch {
    // Ignore storage access errors.
  }
  return DEFAULT_DIFF_MODE;
}

/**
 * Saves the selected diff mode to session storage.
 */
function saveDiffMode(mode: DiffMode): void {
  try {
    window.sessionStorage.setItem(DIFF_MODE_STORAGE_KEY, mode);
  } catch {
    // Ignore storage write errors.
  }
}

/**
 * Handles click events on rewrite-step items to toggle Resolved AST panel
 * visibility and active styling.
 */
export function onRewriteStepClick(event: Event): void {
  const target = event.currentTarget as HTMLElement | null;
  if (!target || target.classList.contains('rewrite-step-active')) {
    return;
  }
  event.preventDefault();
  const stepIndex = target.getAttribute('data-step');
  if (stepIndex === null) {
    return;
  }
  const section = target.closest<HTMLElement>('.rewrite-container');
  if (!section) {
    return;
  }
  if (stepIndex !== '0' && stepIndex !== 'final') {
    setupRewriteStepDiffs(section);
  }

  const panels = section.querySelectorAll<HTMLElement>('.rewrite-ast-panel');
  for (const panel of panels) {
    panel.style.display =
      panel.getAttribute('data-step') === stepIndex ? 'block' : 'none';
  }

  const items = section.querySelectorAll<HTMLElement>('.rewrite-step-item');
  for (const item of items) {
    if (item.getAttribute('data-step') === stepIndex) {
      item.classList.remove('rewrite-step-link');
      item.classList.add('rewrite-step-active');
    } else {
      item.classList.add('rewrite-step-link');
      item.classList.remove('rewrite-step-active');
    }
  }

  const selector = section.querySelector<HTMLElement>('.diff-mode-selector');
  if (selector) {
    selector.style.display =
      stepIndex === '0' || stepIndex === 'final' ? 'none' : '';
  }
}

/**
 * Toggles visibility of the per-mode DOM elements (`.rewrite-ast-none`,
 * `.rewrite-ast-inline`, `.diff-side-by-side`) and updates active button
 * styling within `scope`.
 */
function applyDiffMode(scope: ParentNode, mode: DiffMode): void {
  for (const button of scope.querySelectorAll<HTMLButtonElement>(
    '.diff-mode-btn',
  )) {
    button.classList.toggle(
      'active',
      button.getAttribute('data-mode') === mode,
    );
  }
  for (const el of scope.querySelectorAll<HTMLElement>('.rewrite-ast-none')) {
    el.style.display = mode === 'none' ? '' : 'none';
  }
  for (const el of scope.querySelectorAll<HTMLElement>('.rewrite-ast-inline')) {
    el.style.display = mode === 'inline' ? '' : 'none';
  }
  for (const el of scope.querySelectorAll<HTMLElement>('.diff-side-by-side')) {
    el.style.display = mode === 'side-by-side' ? '' : 'none';
  }
}

/**
 * Wires up click event listeners on the container's `.diff-mode-selector`.
 */
function setupDiffModeSelector(container: HTMLElement): void {
  const modeButtons =
    container.querySelectorAll<HTMLButtonElement>('.diff-mode-btn');
  for (const button of modeButtons) {
    button.addEventListener('click', () => {
      const rawMode = button.getAttribute('data-mode');
      const mode = isDiffMode(rawMode) ? rawMode : DEFAULT_DIFF_MODE;
      saveDiffMode(mode);
      applyDiffMode(document, mode);
    });
  }
}

/**
 * Populates the inline and side-by-side diff DOM elements for a single
 * `.rewrite-container` (leaving `.rewrite-ast-none` untouched) and applies the
 * active diff mode.
 */
function setupRewriteContainerDiffs(container: HTMLElement): void {
  if (container.dataset['diffProcessed']) {
    return;
  }
  const panels = Array.from(
    container.querySelectorAll<HTMLElement>('.rewrite-ast-panel'),
  );
  // At least two steps are required to compute a diff against a previous step.
  if (panels.length < 2) {
    return;
  }
  container.dataset['diffProcessed'] = 'true';

  setupDiffModeSelector(container);

  // Extract the un-diffed AST text and per-line DOM nodes for each step.
  // Intermediate steps store the un-diffed AST in `.rewrite-ast-none`;
  // Pre-rewrite and Post-rewrite panels are `<pre>` elements themselves.
  const panelData = panels.map((panel) => {
    const sourcePre =
      panel.querySelector<HTMLElement>('.rewrite-ast-none') ?? panel;
    const sourceNode =
      sourcePre.querySelector<HTMLElement>('code') ?? sourcePre;
    return {
      panel,
      text: sourceNode.textContent ?? '',
      nodes: splitNodeLines(sourceNode),
    };
  });

  const hasPostRewrite =
    panels[panels.length - 1].getAttribute('data-step') === 'final';
  const lastDiffIndex = hasPostRewrite ? panels.length - 2 : panels.length - 1;

  for (let i = 1; i <= lastDiffIndex; i++) {
    const prev = panelData[i - 1];
    const curr = panelData[i];
    const diff = diffTextLines(prev.text, curr.text, {
      prevNodes: prev.nodes,
      currNodes: curr.nodes,
    });

    const inlineCode = curr.panel.querySelector<HTMLElement>(
      '.rewrite-ast-inline code',
    );
    if (inlineCode) {
      inlineCode.appendChild(renderDiffNodes(diff));
    }

    const sideBySide =
      curr.panel.querySelector<HTMLElement>('.diff-side-by-side');
    if (sideBySide) {
      const {before, after} = renderSideBySideDiffNodes(diff);
      const leftCode = sideBySide.querySelector<HTMLElement>(
        '.diff-pane-before code',
      );
      if (leftCode) {
        leftCode.appendChild(before);
      }

      const rightCode = sideBySide.querySelector<HTMLElement>(
        '.diff-pane-after code',
      );
      if (rightCode) {
        rightCode.appendChild(after);
      }
    }
  }

  applyDiffMode(container, getSavedDiffMode());
}

/**
 * Applies diff formatting (none, inline, or side-by-side) to each intermediate
 * rewrite step relative to the previous step in ".rewrite-container" elements.
 * Pre-rewrite (step 0) and Post-rewrite (final step) show the clean Resolved
 * ASTs.
 * The diff mode setting defaults to 'inline', is synchronized across all
 * statements on the current page, and persists in `sessionStorage` for the
 * current tab session.
 */
export function setupRewriteStepDiffs(
  root: HTMLElement | Document = document,
): void {
  const containers =
    root instanceof HTMLElement && root.classList.contains('rewrite-container')
      ? [root]
      : Array.from(root.querySelectorAll<HTMLElement>('.rewrite-container'));
  for (const container of containers) {
    setupRewriteContainerDiffs(container);
  }
}

/**
 * Initializes default visibility and click listeners for rewrite-step items.
 */
export function initRewriteSteps(rootNode: ParentNode = document): void {
  const sections = rootNode.querySelectorAll('.rewrite-container');
  for (let s = 0; s < sections.length; ++s) {
    const section = sections[s];
    const items = section.querySelectorAll<HTMLElement>('.rewrite-step-item');
    const panels = section.querySelectorAll<HTMLElement>('.rewrite-ast-panel');
    if (items.length === 0 || panels.length === 0) continue;

    for (let i = 0; i < panels.length - 1; ++i) {
      panels[i].style.display = 'none';
    }
    panels[panels.length - 1].style.display = 'block';

    for (let i = 0; i < items.length - 1; ++i) {
      items[i].classList.add('rewrite-step-link');
      items[i].classList.remove('rewrite-step-active');
      items[i].addEventListener('click', onRewriteStepClick);
    }
    items[items.length - 1].classList.remove('rewrite-step-link');
    items[items.length - 1].classList.add('rewrite-step-active');
    items[items.length - 1].addEventListener('click', onRewriteStepClick);

    const selector = section.querySelector<HTMLElement>('.diff-mode-selector');
    if (selector) {
      selector.style.display = 'none';
    }
  }
}
