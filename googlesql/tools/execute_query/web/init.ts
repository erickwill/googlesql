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
 * @fileoverview Main entry point initializing UI components for Execute Query
 * web UI.
 */

import {decorateAllResolvedASTTrees} from './decorate_tree';
import {LocalStorageManager} from './local_storage_manager';
import {initRewriteSteps} from './rewrite_steps';
import {SplitterManager} from './splitter';
import {ThemeManager} from './theme';

/**
 * Initializes web UI components including theme manager, draggable splitter,
 * rewrite step viewer, and resolved AST column hover highlighting.
 */
function init(): void {
  const storageManager = new LocalStorageManager();

  const darkModeToggle = document.getElementById('dark-mode-toggle');
  if (darkModeToggle) {
    new ThemeManager(darkModeToggle, storageManager).init();
  }

  const splitter = document.getElementById('splitter');
  const main = document.querySelector('main');
  const layoutToggle = document.getElementById('layout-toggle');
  if (splitter && main) {
    new SplitterManager(splitter, main, layoutToggle, storageManager).init();
  }

  initRewriteSteps(document);
  decorateAllResolvedASTTrees(document);
}

if (typeof document !== 'undefined') {
  init();
}
