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
 * @fileoverview Computes and renders line-by-line diffs for Resolved AST trees.
 */

/**
 * Represents a single line in a diff computation between two Resolved ASTs.
 */
export interface DiffLineResult {
  /** True if the line was added in the current Resolved AST. */
  readonly added?: boolean;
  /** True if the line was removed from the previous Resolved AST. */
  readonly removed?: boolean;
  /**
   * Plain text content of the line (from the current Resolved AST, or from the
   * previous Resolved AST if removed).
   */
  readonly text: string;
  /**
   * Optional DOM node fragment for the line, preserving syntax highlighting
   * and column markup.
   */
  readonly nodes?: DocumentFragment;
  /**
   * For unchanged lines, the original plain text from the previous Resolved
   * AST (which may differ in tree connectors or indentation from `text`).
   */
  readonly prevText?: string;
  /**
   * For unchanged lines, the original DOM node fragment from the previous
   * Resolved AST.
   */
  readonly prevNodes?: DocumentFragment;
}

const LEADING_DIFF_OR_TREE_CHARS_REGEX =
  /^(?:[+\-](?![0-9])|[\s\u00A0\u2000-\u200B\u2500-\u257F\u00B7\u2022\u2026\u25A0-\u25FF|\\/>.~])+/;

/**
 * Strips leading whitespace and tree-drawing characters (such as box-drawing
 * glyphs '├', '─', '│', '└', as well as ASCII tree branch connectors and
 * spaces) so that lines matching in content are not treated as diffs when only
 * indentation or parent nesting changes.
 */
export function normalizeLineForDiff(line: string): string {
  return line.replace(LEADING_DIFF_OR_TREE_CHARS_REGEX, '').trim();
}

/**
 * Splits the contents of a DOM node into per-line DocumentFragments separated
 * by '\n', preserving inline element wrappers across line boundaries by
 * cloning open ancestor elements for each line.
 */
export function splitNodeLines(root: Node): DocumentFragment[] {
  const lines: DocumentFragment[] = [];
  let currentLine = document.createDocumentFragment();
  const ancestorTemplates: Element[] = [];
  let currentContainers: Array<DocumentFragment | Element> = [currentLine];

  function getTargetContainer(): DocumentFragment | Element {
    while (currentContainers.length <= ancestorTemplates.length) {
      const ancestor = ancestorTemplates[currentContainers.length - 1];
      const clone = ancestor.cloneNode(false);
      if (clone instanceof Element) {
        currentContainers[currentContainers.length - 1].appendChild(clone);
        currentContainers.push(clone);
      }
    }
    return currentContainers[currentContainers.length - 1];
  }

  function startNewLine(): void {
    lines.push(currentLine);
    currentLine = document.createDocumentFragment();
    currentContainers = [currentLine];
  }

  function visit(node: Node): void {
    if (node.nodeType === Node.TEXT_NODE) {
      const text = node.textContent ?? '';
      const parts = text.split('\n');
      for (let i = 0; i < parts.length; i++) {
        if (i > 0) {
          startNewLine();
        }
        if (parts[i].length > 0) {
          getTargetContainer().appendChild(document.createTextNode(parts[i]));
        }
      }
    } else if (node instanceof Element) {
      ancestorTemplates.push(node);
      if (node.childNodes.length === 0) {
        getTargetContainer();
      } else {
        for (const child of Array.from(node.childNodes)) {
          visit(child);
        }
      }
      ancestorTemplates.pop();
      if (currentContainers.length > ancestorTemplates.length + 1) {
        currentContainers.length = ancestorTemplates.length + 1;
      }
    }
  }

  for (const child of Array.from(root.childNodes)) {
    visit(child);
  }
  lines.push(currentLine);
  return lines;
}

/**
 * Optional pre-split DOM node fragments for each line of the previous and
 * current Resolved ASTs.
 */
export interface DiffTextLinesOptions {
  readonly prevNodes?: readonly DocumentFragment[];
  readonly currNodes?: readonly DocumentFragment[];
}

/**
 * Computes a line-by-line diff between two strings representing Resolved ASTs
 * using a standard Longest Common Subsequence (LCS) algorithm.
 *
 * A custom line-level diff is implemented here rather than importing an
 * external library (such as jsdiff) for the following reasons:
 * 1. External npm diff packages are not available in the open-source esbuild
 *    bundle for this web target.
 * 2. Tree-prefix normalization: `normalizeLineForDiff()` strips leading
 *    whitespace and tree-drawing characters (e.g. `│ ├─`, `+--`, `└─`) so that
 *    changes in surrounding parent hierarchy or sibling indentation do not
 *    flag unchanged Resolved AST nodes as diffs.
 * 3. DOM preservation: Each line's pre-rendered DOM nodes (syntax highlighting
 *    `hljs-*` classes and interactive `ast-col` / `ast-range` spans) are
 *    preserved line-by-line in the diff output without re-parsing HTML.
 */
export function diffTextLines(
  prevText: string,
  currText: string,
  options?: DiffTextLinesOptions,
): DiffLineResult[] {
  const prevLines = prevText.split('\n');
  const currLines = currText.split('\n');
  const normalizedPrevLines = prevLines.map(normalizeLineForDiff);
  const normalizedCurrLines = currLines.map(normalizeLineForDiff);
  const prevNodes = options?.prevNodes;
  const currNodes = options?.currNodes;

  const prevLineCount = prevLines.length;
  const currLineCount = currLines.length;

  const lcsLengths: number[][] = Array.from({length: prevLineCount + 1}, () =>
    new Array<number>(currLineCount + 1).fill(0),
  );
  for (let i = 0; i < prevLineCount; i++) {
    for (let j = 0; j < currLineCount; j++) {
      lcsLengths[i + 1][j + 1] =
        normalizedPrevLines[i] === normalizedCurrLines[j]
          ? lcsLengths[i][j] + 1
          : Math.max(lcsLengths[i + 1][j], lcsLengths[i][j + 1]);
    }
  }

  let i = prevLineCount;
  let j = currLineCount;
  const diff: DiffLineResult[] = [];

  while (i > 0 || j > 0) {
    if (
      i > 0 &&
      j > 0 &&
      normalizedPrevLines[i - 1] === normalizedCurrLines[j - 1]
    ) {
      const currLineNodes = currNodes?.[j - 1];
      const prevLineNodes = prevNodes?.[i - 1];
      const prevLine = prevLines[i - 1];
      const currLine = currLines[j - 1];
      diff.push({
        text: currLine,
        ...(prevLine !== currLine ? {prevText: prevLine} : {}),
        ...(currLineNodes != null ? {nodes: currLineNodes} : {}),
        ...(prevLineNodes != null ? {prevNodes: prevLineNodes} : {}),
      });
      i--;
      j--;
    } else if (
      j > 0 &&
      (i === 0 || lcsLengths[i][j - 1] >= lcsLengths[i - 1][j])
    ) {
      const currLineNodes = currNodes?.[j - 1];
      diff.push(
        currLineNodes != null
          ? {added: true, text: currLines[j - 1], nodes: currLineNodes}
          : {added: true, text: currLines[j - 1]},
      );
      j--;
    } else if (
      i > 0 &&
      (j === 0 || lcsLengths[i][j - 1] < lcsLengths[i - 1][j])
    ) {
      const prevLineNodes = prevNodes?.[i - 1];
      diff.push(
        prevLineNodes != null
          ? {removed: true, text: prevLines[i - 1], nodes: prevLineNodes}
          : {removed: true, text: prevLines[i - 1]},
      );
      i--;
    }
  }

  diff.reverse();
  return diff;
}

/**
 * Renders diff line results into a DocumentFragment containing styled DOM
 * nodes.
 */
export function renderDiffNodes(
  diffResults: readonly DiffLineResult[],
): DocumentFragment {
  const fragment = document.createDocumentFragment();
  for (const [index, item] of diffResults.entries()) {
    if (item.added || item.removed) {
      const span = document.createElement('span');
      span.className = item.added ? 'diff-added' : 'diff-removed';
      if (item.nodes != null) {
        span.appendChild(item.nodes.cloneNode(true));
      } else {
        span.textContent = item.text;
      }
      fragment.appendChild(span);
    } else if (item.nodes != null) {
      fragment.appendChild(item.nodes.cloneNode(true));
    } else {
      fragment.appendChild(document.createTextNode(item.text));
    }
    if (index < diffResults.length - 1) {
      fragment.appendChild(document.createTextNode('\n'));
    }
  }
  return fragment;
}

/**
 * Renders diff line results into separate Before and After DocumentFragments
 * for side-by-side diff viewing. Unchanged lines in the Before fragment use
 * the previous step's text and DOM nodes so tree indentation stays aligned.
 */
export function renderSideBySideDiffNodes(
  diffResults: readonly DiffLineResult[],
): {before: DocumentFragment; after: DocumentFragment} {
  const beforeDiff: DiffLineResult[] = diffResults
    .filter((item) => !item.added)
    .map((item) => {
      const nodes = item.prevNodes ?? item.nodes;
      const text = item.prevText ?? item.text;
      return {
        ...(item.removed ? {removed: true} : {}),
        text,
        ...(nodes != null ? {nodes} : {}),
      };
    });
  const afterDiff: DiffLineResult[] = diffResults.filter(
    (item) => !item.removed,
  );
  return {
    before: renderDiffNodes(beforeDiff),
    after: renderDiffNodes(afterDiff),
  };
}
