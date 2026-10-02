import AppKit

@MainActor
final class FilesAccessibilityTextLayout {
    private let layoutManager: NSTextLayoutManager
    private var lineCache: (generation: Int, width: CGFloat, lines: [(NSRange, CGRect)])?

    init(layoutManager: NSTextLayoutManager) { self.layoutManager = layoutManager }

    func query(_ query: OuterframeAccessibilityTextQuery, range: NSRange, point: CGPoint,
               text: String, generation: Int, viewport: CGRect, scrollOffset: CGFloat, inset: CGSize) -> OuterframeAccessibilityTextResult? {
        let length = text.utf16.count
        func rootFrame(_ rect: CGRect) -> CGRect {
            CGRect(x: viewport.minX + inset.width + rect.minX,
                   y: viewport.maxY - inset.height - rect.maxY + scrollOffset,
                   width: max(rect.width, 1), height: max(rect.height, 1))
        }
        switch query {
        case .frameForRange:
            guard range.location >= 0, range.length >= 0, range.location <= length, range.length <= length - range.location,
                  OuterframeAccessibilityTextRange.isValid(range, in: text),
                  let start = layoutManager.location(layoutManager.documentRange.location, offsetBy: range.location),
                  let end = layoutManager.location(start, offsetBy: range.length),
                  let textRange = NSTextRange(location: start, end: end) else { return nil }
            layoutManager.ensureLayout(for: textRange)
            var bounds = CGRect.null
            layoutManager.enumerateTextSegments(in: textRange, type: range.length == 0 ? .standard : .selection, options: []) { _, rect, _, _ in
                bounds = bounds.union(rect)
                return true
            }
            return bounds.isNull ? nil : OuterframeAccessibilityTextResult(frame: rootFrame(bounds))
        case .rangeForPosition:
            guard viewport.contains(point) else { return nil }
            let textPoint = CGPoint(x: max(point.x - viewport.minX - inset.width, 0),
                                    y: max(viewport.maxY - point.y + scrollOffset - inset.height, 0))
            layoutManager.ensureLayout(for: layoutManager.documentRange)
            guard let fragment = layoutManager.textLayoutFragment(for: textPoint),
                  let line = fragment.textLineFragment(forVerticalOffset: textPoint.y - fragment.layoutFragmentFrame.minY, requiresExactMatch: false) else {
                return OuterframeAccessibilityTextResult(range: NSRange(location: textPoint.y <= 0 ? 0 : length, length: 0))
            }
            let base = layoutManager.offset(from: layoutManager.documentRange.location, to: fragment.rangeInElement.location)
            let localPoint = CGPoint(x: textPoint.x - fragment.layoutFragmentFrame.minX - line.typographicBounds.minX,
                                     y: textPoint.y - fragment.layoutFragmentFrame.minY - line.typographicBounds.minY)
            let offset = min(max(base + line.characterIndex(for: localPoint), 0), length)
            let result = offset < length ? (text as NSString).rangeOfComposedCharacterSequence(at: offset) : NSRange(location: length, length: 0)
            return OuterframeAccessibilityTextResult(range: result)
        case .lineForIndex:
            guard range.location >= 0, range.location <= length else { return nil }
            let lines = lines(textLength: length, generation: generation)
            guard let index = lines.lastIndex(where: { $0.0.location <= range.location }) else { return nil }
            return OuterframeAccessibilityTextResult(index: index)
        case .rangeForLine:
            let lines = lines(textLength: length, generation: generation)
            guard lines.indices.contains(range.location) else { return nil }
            return OuterframeAccessibilityTextResult(range: lines[range.location].0)
        case .visibleRange:
            let visible = lines(textLength: length, generation: generation).filter { rootFrame($0.1).intersects(viewport) }
            guard let first = visible.first, let last = visible.last else {
                return OuterframeAccessibilityTextResult(range: NSRange(location: 0, length: 0))
            }
            return OuterframeAccessibilityTextResult(range: NSRange(location: first.0.location, length: NSMaxRange(last.0) - first.0.location))
        }
    }

    private func lines(textLength: Int, generation: Int) -> [(NSRange, CGRect)] {
        let width = layoutManager.textContainer?.size.width ?? 0
        if let cache = lineCache, cache.generation == generation, cache.width == width { return cache.lines }
        layoutManager.ensureLayout(for: layoutManager.documentRange)
        var lines: [(NSRange, CGRect)] = []
        layoutManager.enumerateTextLayoutFragments(from: layoutManager.documentRange.location, options: [.ensuresLayout, .ensuresExtraLineFragment]) { fragment in
            let base = self.layoutManager.offset(from: self.layoutManager.documentRange.location, to: fragment.rangeInElement.location)
            for line in fragment.textLineFragments {
                let start = base + line.characterRange.location
                guard start >= 0, start <= textLength else { continue }
                let range = NSRange(location: start, length: min(line.characterRange.length, textLength - start))
                let bounds = line.typographicBounds.offsetBy(dx: fragment.layoutFragmentFrame.minX, dy: fragment.layoutFragmentFrame.minY)
                lines.append((range, bounds))
            }
            return true
        }
        if lines.isEmpty { lines.append((NSRange(location: 0, length: 0), .zero)) }
        lineCache = (generation, width, lines)
        return lines
    }
}
