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
 * @fileoverview Decorates the HTML of the resolved AST.
 */

/**
 * Interface for highlighting a byte offset range in the query editor.
 */
export interface RangeHighlighter {
  highlightRangeWithByteOffsets(start: number, end: number): void;
  clearHighlights(): void;
}

function getMatchingColumnNodes(
  targetElement: HTMLElement,
  preElement: HTMLElement,
): NodeListOf<Element> | null {
  const columnNode = targetElement.closest('.ast-col');
  if (!columnNode) {
    return null;
  }

  const selector = `.${Array.from(columnNode.classList).join('.')}`;
  return preElement.querySelectorAll(selector);
}

function addClassToNodes(nodes: NodeListOf<Element>, className: string) {
  nodes.forEach((node) => {
    node.classList.add(className);
  });
}

function removeClassFromNodes(nodes: NodeListOf<Element>, className: string) {
  nodes.forEach((node) => {
    node.classList.remove(className);
  });
}

/**
 * Sets up event listeners to highlight matching nodes when the mouse interacts
 * with a column ID node.
 */
export function setupColumnHighlighting(preElement: HTMLElement) {
  // Mouseover for temporary highlighting.
  preElement.addEventListener('mouseover', (event) => {
    if (!(event.target instanceof HTMLElement)) {
      return;
    }

    if (event.target.closest('.ast-col')) {
      const stickyNodes = preElement.querySelectorAll('.highlighted-sticky');
      removeClassFromNodes(stickyNodes, 'highlighted-sticky');
      addClassToNodes(stickyNodes, 'highlighted-sticky-suspended');
    }

    const matchingNodes = getMatchingColumnNodes(event.target, preElement);
    if (matchingNodes) {
      addClassToNodes(matchingNodes, 'highlighted');
    }
  });

  // Mouseout to remove temporary highlighting.
  preElement.addEventListener('mouseout', (event) => {
    if (
      !(event.target instanceof HTMLElement) ||
      !event.target.closest('.ast-col')
    ) {
      return;
    }

    const highlightedNodes = preElement.querySelectorAll('.highlighted');
    removeClassFromNodes(highlightedNodes, 'highlighted');

    const suspendedStickyNodes = preElement.querySelectorAll(
      '.highlighted-sticky-suspended',
    );
    removeClassFromNodes(suspendedStickyNodes, 'highlighted-sticky-suspended');
    addClassToNodes(suspendedStickyNodes, 'highlighted-sticky');
  });

  // Mousedown for sticky highlighting.
  preElement.addEventListener('mousedown', (event) => {
    if (!(event.target instanceof HTMLElement)) {
      return;
    }

    // Remove any existing sticky highlights.
    const stickyNodes = preElement.querySelectorAll(
      '.highlighted-sticky, .highlighted-sticky-suspended',
    );
    stickyNodes.forEach((node) => {
      node.classList.remove(
        'highlighted-sticky',
        'highlighted-sticky-suspended',
      );
    });

    // Add sticky highlight to the new set of matching nodes.
    const matchingNodes = getMatchingColumnNodes(event.target, preElement);
    if (matchingNodes) {
      addClassToNodes(matchingNodes, 'highlighted-sticky');
    }
  });
}

/**
 * Sets up event listeners to highlight the range in the editor when the mouse
 * interacts with a parsed AST node.
 */
export function setupRangeHighlighting(
  preElement: HTMLElement,
  rangeHighlighter: RangeHighlighter,
) {
  preElement.addEventListener('mouseover', (event) => {
    if (!(event.target instanceof HTMLElement)) {
      return;
    }

    const rangeNode = event.target.closest('.ast-range');
    if (!rangeNode) {
      return;
    }
    const text = rangeNode.textContent;
    if (!text) {
      return;
    }

    // format: [start-end]
    const match = text.match(/.*\[(\d+)-(\d+)\]/);
    if (match) {
      const start = Number(match[1]);
      const end = Number(match[2]);
      rangeHighlighter.highlightRangeWithByteOffsets(start, end);
      return;
    }

    // format: start-end
    const matchNoBrackets = text.match(/parse_location=(\d+)-(\d+)/);
    if (matchNoBrackets) {
      const start = Number(matchNoBrackets[1]);
      const end = Number(matchNoBrackets[2]);
      rangeHighlighter.highlightRangeWithByteOffsets(start, end);
      return;
    }
  });

  preElement.addEventListener('mouseout', (event) => {
    if (
      event.target instanceof HTMLElement &&
      event.target.closest('.ast-range')
    ) {
      rangeHighlighter.clearHighlights();
    }
  });
}

/**
 * Decorates all resolved AST trees in the document with hover highlighting
 * over column ID nodes.
 */
export function decorateAllResolvedASTTrees(doc: Document = document) {
  const astTrees = doc.querySelectorAll('.analyzed, .error');
  for (const astTree of astTrees) {
    if (astTree instanceof HTMLElement) {
      setupColumnHighlighting(astTree);
    }
  }
}

/**
 * Decorates all resolved and parsed AST trees in the document with editor
 * byte-range hover highlighting.
 */
export function decorateRangeHighlighting(
  rangeHighlighter: RangeHighlighter,
  doc: Document = document,
) {
  const trees = doc.querySelectorAll('.analyzed, .output.parsed, .error');
  for (const tree of trees) {
    if (tree instanceof HTMLElement) {
      setupRangeHighlighting(tree, rangeHighlighter);
    }
  }
}
