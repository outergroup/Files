import AppKit
import Darwin
import Foundation
import QuartzCore
import UniformTypeIdentifiers

@MainActor
@objc public final class FilesContent: NSObject, OuterframeContentLibrary {
    @objc public static func start(
        socketFD: Int32,
        appConnection: OuterframeAppConnection
    ) -> Int32 {
        let outerframeHost = OuterframeHost(socketFD: socketFD)
        let handler = FilesHandler(outerframeHost: outerframeHost, appConnection: appConnection)
        outerframeHost.delegate = handler
        return 0
    }
}

private struct FileListResponse: Sendable {
    let path: String
    let parent: String?
    let entries: [FileEntry]
}

private struct FileEntry: Sendable {
    let name: String
    let path: String
    let isDirectory: Bool
    let size: UInt64
    let modified: Double
    let mode: String
    let accessFlags: UInt32

    var userCanView: Bool {
        (accessFlags & 1) != 0
    }

    var userCanModify: Bool {
        (accessFlags & 2) != 0
    }

    var isExecutableFile: Bool {
        guard !isDirectory,
              mode.count >= 10,
              mode.first == "-" else {
            return false
        }
        let characters = Array(mode)
        return characters[3] == "x" ||
               characters[3] == "s" ||
               characters[6] == "x" ||
               characters[6] == "s" ||
               characters[9] == "x" ||
               characters[9] == "t"
    }
}

private struct DragPreview {
    let pngData: Data
    let size: CGSize
}

private struct FileOpener: Sendable {
    let contentType: String
    let serviceID: String
    let displayName: String
    let socketPath: String
    let url: String
    let ownerName: String
}

private struct FileOpenMenuAction {
    let title: String
    let opener: FileOpener
}

private enum FileOpenersFetchResult {
    case success([FileOpener])
    case failure(String)
}

private enum FileOpenersBinaryFormat {
    static let magic: UInt32 = 0x504f464f
    static let requestMagic: UInt32 = 0x514f464f
    static let version: UInt32 = 2
    static let headerSize = 32
    static let rowSize = 48
}

private enum FilePathRequestBinaryFormat {
    static let magic: UInt32 = 0x51465046
    static let version: UInt32 = 1
}

private enum FileMkdirRequestBinaryFormat {
    static let magic: UInt32 = 0x51444d46
    static let version: UInt32 = 1
}

private enum FileListBinaryFormat {
    static let magic: UInt32 = 0x534c4646
    static let version: UInt32 = 2
    static let headerSize = 48
    static let rowSize = 48
}

private struct BreadcrumbSegment {
    let title: String
    let path: String
}

private struct FavoriteLocation: Sendable {
    let title: String
    let path: String
}

private struct DroppedFileAccessPayload: Sendable {
    let id: UUID
    let name: String
    let localPath: String
    let fileSize: UInt64?
    let fileType: String?
    let isDirectory: Bool
}

private struct DroppedLocalFile: Sendable {
    let id: UUID
    let fileURL: URL
    let name: String
    let fileSize: UInt64?
    let fileType: String?
    let isDirectory: Bool
}

private enum OuterframePasteboardPayload {
    private static let version: UInt32 = 1
    private static let droppedFileAccessMissingSize = UInt64.max
    private static let droppedFileAccessDirectoryFlag: UInt32 = 1 << 0

    static func decodeDroppedFileAccess(_ data: Data) -> DroppedFileAccessPayload? {
        var cursor = BinaryPayloadCursor(data)
        guard cursor.readUInt32() == version,
              let flags = cursor.readUInt32(),
              let id = cursor.readUUID(),
              let encodedFileSize = cursor.readUInt64(),
              let name = cursor.readStringReference(),
              !name.isEmpty,
              let fileType = cursor.readStringReference(),
              let localPath = cursor.readStringReference(),
              !localPath.isEmpty else {
            return nil
        }

        return DroppedFileAccessPayload(id: id,
                                        name: name,
                                        localPath: localPath,
                                        fileSize: encodedFileSize == droppedFileAccessMissingSize ? nil : encodedFileSize,
                                        fileType: fileType.isEmpty ? nil : fileType,
                                        isDirectory: flags & droppedFileAccessDirectoryFlag != 0)
    }
}

private struct BinaryPayloadBuilder {
    private struct Reference {
        let patchOffset: Int
        let variableOffset: Int
        let length: Int
    }

    private var fixed = Data()
    private var variable = Data()
    private var references: [Reference] = []
    private let referenceBaseOffset: Int

    init(referenceBaseOffset: Int) {
        self.referenceBaseOffset = referenceBaseOffset
    }

    mutating func append(uint32 value: UInt32) {
        fixed.appendLittleEndian(value)
    }

    mutating func append(stringReference string: String) -> Bool {
        guard let data = string.data(using: .utf8),
              data.count <= Int(UInt32.max) else {
            return false
        }
        let patchOffset = fixed.count
        fixed.appendLittleEndian(UInt32(0))
        fixed.appendLittleEndian(UInt32(data.count))
        references.append(Reference(patchOffset: patchOffset,
                                    variableOffset: variable.count,
                                    length: data.count))
        variable.append(data)
        return true
    }

    mutating func finalize() -> Data? {
        guard fixed.count <= Int(UInt32.max),
              variable.count <= Int(UInt32.max),
              variable.count <= Int(UInt32.max) - fixed.count else {
            return nil
        }

        for reference in references {
            let offset = referenceBaseOffset + fixed.count + reference.variableOffset
            guard offset <= Int(UInt32.max),
                  reference.length <= Int(UInt32.max) else {
                return nil
            }
            fixed.replaceLittleEndianUInt32(at: reference.patchOffset, with: UInt32(offset))
            fixed.replaceLittleEndianUInt32(at: reference.patchOffset + 4, with: UInt32(reference.length))
        }

        var payload = Data(capacity: fixed.count + variable.count)
        payload.append(fixed)
        payload.append(variable)
        return payload
    }
}

private struct BinaryPayloadCursor {
    private let data: Data
    private var offset = 0

    init(_ data: Data, offset: Int = 0) {
        self.data = data
        self.offset = offset
    }

    mutating func readUInt32() -> UInt32? {
        guard offset + 4 <= data.count else { return nil }
        var value: UInt32 = 0
        for index in 0..<4 {
            value |= UInt32(data[offset + index]) << UInt32(index * 8)
        }
        offset += 4
        return value
    }

    mutating func readUInt64() -> UInt64? {
        guard offset + 8 <= data.count else { return nil }
        var value: UInt64 = 0
        for index in 0..<8 {
            value |= UInt64(data[offset + index]) << UInt64(index * 8)
        }
        offset += 8
        return value
    }

    mutating func readUUID() -> UUID? {
        guard offset + 16 <= data.count else { return nil }
        let bytes = data.subdata(in: offset..<(offset + 16))
        offset += 16
        return bytes.withUnsafeBytes { raw -> UUID? in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return nil }
            return NSUUID(uuidBytes: base) as UUID
        }
    }

    mutating func readStringReference() -> String? {
        guard let data = readDataReference() else { return nil }
        return String(data: data, encoding: .utf8)
    }

    private mutating func readDataReference() -> Data? {
        guard let offsetValue = readUInt32(),
              let lengthValue = readUInt32() else {
            return nil
        }

        let start = Int(offsetValue)
        let length = Int(lengthValue)
        guard start <= data.count,
              length <= data.count - start else {
            return nil
        }
        return data.subdata(in: start..<(start + length))
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
    }

    mutating func replaceLittleEndianUInt32(at offset: Int, with value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) {
            replaceSubrange(offset..<(offset + 4), with: $0)
        }
    }
}

private final class FilesHandler: NSObject, OuterframeHostDelegate {
    private static let droppedFileAccessPasteboardTypeIdentifier = "org.outerframe.dropped-file-access"

    private let outerframeHost: OuterframeHost
    private let appConnection: OuterframeAppConnection
    private var retainedSelf: FilesHandler?

    private let rootLayer = CALayer()
    private let favoritesBarLayer = CALayer()
    private let breadcrumbBarLayer = CALayer()
    private let headerLayer = CALayer()
    private let nameHeaderLayer = CATextLayer()
    private let modifiedHeaderLayer = CATextLayer()
    private let sizeHeaderLayer = CATextLayer()
    private let rowsClipLayer = CALayer()
    private let rowsContentLayer = CALayer()
    private let statusLayer = CATextLayer()

    private var appearance = NSAppearance.currentDrawing()
    private var currentSize = CGSize(width: 900, height: 600)
    private var urlSession: URLSession?
    private var filesEndpoint: URL?
    private var openersEndpoint: URL?
    private var downloadEndpoint: URL?
    private var uploadEndpoint: URL?
    private var mkdirEndpoint: URL?
    private var currentPath = "~"
    private var homePath: String?
    private var parentPath: String?
    private var entries: [FileEntry] = []
    private var selectedIndex: Int?
    private var dragCandidateIndex: Int?
    private var dragStartPoint: CGPoint?
    private var dragStartedForSelectionIndex: Int?
    private var isDraggingFolderToFavorites = false
    private var filePromiseEntries: [UUID: FileEntry] = [:]
    private var favoriteLocations: [FavoriteLocation] = []
    private var scrollOffset: CGFloat = 0
    private var isLoading = false
    private var hasRegisteredLayer = false
    private var shouldReplaceHistoryEntryAfterLoad = false
    private var favoriteFrames: [(frame: CGRect, path: String)] = []
    private var breadcrumbSegmentFrames: [(frame: CGRect, path: String)] = []
    private lazy var rowsScrollbarDelegate = FilesRowsScrollbarDelegate(owner: self)
    private var rowsScrollbarController: ScrollbarController<FilesRowsScrollbarDelegate>?
    private var visibleRowLayers: [Int: CALayer] = [:]
    private var reusableRowLayers: [CALayer] = []
    private var pendingDirectoryMenuEntries: [UUID: FileEntry] = [:]
    private var pendingOpenMenuEntries: [UUID: (entry: FileEntry, openers: [FileOpener])] = [:]
    private var resizeLayoutUpdateScheduled = false
    private var accessibilityNotificationScheduled = false
    private var typeaheadPrefix = ""
    private var typeaheadLastUpdated: Date?
    private var suppressNextMouseUpAfterControlClick = false
    private var terminalIconCache: [String: CGImage] = [:]
    private let iconContentsScale: CGFloat = 2

    private let favoritesBarHeight: CGFloat = 36
    private let breadcrumbBarHeight: CGFloat = 34
    private let headerHeight: CGFloat = 28
    private let rowHeight: CGFloat = 26
    private let horizontalInset: CGFloat = 18
    private let nameColumnWidth: CGFloat = 0.58
    private let modifiedColumnWidth: CGFloat = 0.24

    private var topChromeHeight: CGFloat {
        favoritesBarHeight + breadcrumbBarHeight
    }

    init(outerframeHost: OuterframeHost, appConnection: OuterframeAppConnection) {
        self.outerframeHost = outerframeHost
        self.appConnection = appConnection
        super.init()
        retainedSelf = self
    }

    func outerframeHost(_ host: OuterframeHost, didReceiveMessage message: BrowserToContentMessage) {
        switch message {
        case .initializeContent(let arguments):
            outerframeHost.configure(url: arguments.url ?? "",
                                     bundleUrl: arguments.bundleUrl ?? "",
                                     proxyHost: arguments.proxy?.host,
                                     proxyPort: arguments.proxy?.port ?? 0,
                                     proxyUsername: arguments.proxy?.username,
                                     proxyPassword: arguments.proxy?.password)
            outerframeHost.setTitle("Files")
            outerframeHost.setIcon(.bundleResource(path: "Contents/Resources/app-icon.png"))
            appearance = arguments.appearance ?? appearance
            currentSize = arguments.contentSize ?? currentSize
            configureNetworking()
            configureLayersIfNeeded()
            updateColors()
            updateLayout()
            registerRootLayerIfNeeded()
            outerframeHost.setInputMode(.rawKeys)
            updatePasteboardCapabilities()
            let initialURLPath = pathFromURL(arguments.url)
            let initialPath = initialURLPath ?? currentPath
            fetchFiles(path: initialPath,
                       replaceHistoryEntryAfterLoad: initialURLPath == nil)

        case .resizeContent(let size):
            currentSize = size
            scheduleResizeLayoutUpdate()

        case .systemAppearanceUpdate(let appearance):
            self.appearance = appearance
            updateColors()

        case .scrollWheelEvent(let point, let delta, _, _, _, let hasPreciseScrollingDeltas):
            guard rowsClipLayer.frame.contains(rootLayer.convert(point, to: rowsClipLayer.superlayer)) else { return }
            let multiplier: CGFloat = hasPreciseScrollingDeltas ? 1 : rowHeight
            setRowsScroll(scrollOffset - delta.y * multiplier)

        case .mouseDown(let point, let modifierFlags, let clickCount):
            if modifierFlags.contains(.control) {
                suppressNextMouseUpAfterControlClick = true
                handleRightMouseDown(at: point)
            } else {
                suppressNextMouseUpAfterControlClick = false
                handleMouseDown(at: point, clickCount: clickCount)
            }

        case .mouseDragged(let point, let modifierFlags):
            handleMouseDragged(to: point, modifierFlags: modifierFlags)

        case .mouseUp(let point, _):
            if suppressNextMouseUpAfterControlClick {
                suppressNextMouseUpAfterControlClick = false
            } else {
                handleMouseUp(at: point)
            }

        case .rightMouseDown(let point, _, _):
            handleRightMouseDown(at: point)

        case .keyDown(let keyCode, let characters, _, _, _):
            handleKeyDown(keyCode: keyCode, characters: characters)

        case .selectionToPasteboardCopyRequest(let requestID):
            handleSelectionToPasteboardCopyRequest(requestID: requestID)

        case .selectionToPasteboardCutRequest(let requestID):
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID, items: [])

        case .editCommandValidationRequest(let requestID, let commands):
            outerframeHost.sendEditCommandValidationResponse(
                requestID: requestID,
                enabledCommands: enabledEditCommands(in: commands)
            )

        case .pasteboardContentPasted(let items):
            handleDroppedPasteboardItems(items, at: CGPoint(x: rowsClipLayer.bounds.midX, y: rowsClipLayer.bounds.midY))

        case .pasteboardContentDropped(let point, let items):
            handleDroppedPasteboardItems(items, at: point)

        case .filePromiseWriteRequest(let requestID, let promiseID):
            handleFilePromiseWriteRequest(requestID: requestID, promiseID: promiseID)

        case .contextMenuItemSelected(let menuID, let itemID):
            handleContextMenuItemSelected(menuID: menuID, itemID: itemID)

        case .historyTraversal(_, let url):
            fetchFiles(path: pathFromURL(url) ?? currentPath)

        case .accessibilitySnapshotRequest(let requestID):
            outerframeHost.sendAccessibilitySnapshotResponse(requestID: requestID,
                                                             snapshot: buildAccessibilitySnapshot())

        case .shutdown:
            retainedSelf = nil

        default:
            break
        }
    }

    func outerframeHostDidDisconnect(_ host: OuterframeHost) {
        retainedSelf = nil
    }

    private func configureNetworking() {
        if let base = outerframeHost.pluginBaseURL() {
            filesEndpoint = URL(string: "/api/files", relativeTo: base)?.absoluteURL
            openersEndpoint = URL(string: "/api/openers", relativeTo: base)?.absoluteURL
            downloadEndpoint = URL(string: "/api/download", relativeTo: base)?.absoluteURL
            uploadEndpoint = URL(string: "/api/upload", relativeTo: base)?.absoluteURL
            mkdirEndpoint = URL(string: "/api/mkdir", relativeTo: base)?.absoluteURL
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 5
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: configuration)
        urlSession = URLSession(configuration: configuration)
    }

    private func configureLayersIfNeeded() {
        guard favoritesBarLayer.superlayer == nil else { return }

        rootLayer.masksToBounds = true
        rootLayer.addSublayer(favoritesBarLayer)
        rootLayer.addSublayer(breadcrumbBarLayer)
        rootLayer.addSublayer(headerLayer)
        rootLayer.addSublayer(rowsClipLayer)
        rootLayer.addSublayer(statusLayer)
        rowsClipLayer.masksToBounds = true
        rowsClipLayer.addSublayer(rowsContentLayer)
        let scrollbar = ScrollbarController<FilesRowsScrollbarDelegate>(appConnection: outerframeHost,
                                                                        viewportLayer: rowsClipLayer,
                                                                        appearance: appearance,
                                                                        width: 8,
                                                                        inset: 4,
                                                                        scrollOffsetOrigin: .bottom)
        scrollbar.delegate = rowsScrollbarDelegate
        rowsScrollbarController = scrollbar

        headerLayer.addSublayer(nameHeaderLayer)
        headerLayer.addSublayer(modifiedHeaderLayer)
        headerLayer.addSublayer(sizeHeaderLayer)

        for layer in [nameHeaderLayer, modifiedHeaderLayer, sizeHeaderLayer, statusLayer] {
            layer.font = NSFont.systemFont(ofSize: 12, weight: .medium)
            layer.fontSize = 12
            layer.contentsScale = 2
            layer.truncationMode = .end
        }
        nameHeaderLayer.string = "Name"
        modifiedHeaderLayer.string = "Modified"
        sizeHeaderLayer.string = "Size"
        sizeHeaderLayer.alignmentMode = .right
        statusLayer.alignmentMode = .center
    }

    private func updateLayout() {
        withoutImplicitAnimations {
            let width = max(currentSize.width, 1)
            let height = max(currentSize.height, 1)
            rootLayer.frame = CGRect(origin: .zero, size: CGSize(width: width, height: height))

            favoritesBarLayer.frame = CGRect(x: 0,
                                             y: max(height - favoritesBarHeight, 0),
                                             width: width,
                                             height: favoritesBarHeight)
            breadcrumbBarLayer.frame = CGRect(x: 0,
                                              y: max(height - topChromeHeight, 0),
                                              width: width,
                                              height: breadcrumbBarHeight)

            let headerY = max(height - topChromeHeight - headerHeight, 0)
            headerLayer.frame = CGRect(x: 0, y: headerY, width: width, height: headerHeight)
            let contentWidth = max(width - horizontalInset * 2, 1)
            let nameWidth = floor(contentWidth * nameColumnWidth)
            let modifiedWidth = floor(contentWidth * modifiedColumnWidth)
            let sizeWidth = max(contentWidth - nameWidth - modifiedWidth, 1)
            nameHeaderLayer.frame = CGRect(x: horizontalInset, y: 7, width: nameWidth, height: 16)
            modifiedHeaderLayer.frame = CGRect(x: horizontalInset + nameWidth, y: 7, width: modifiedWidth, height: 16)
            sizeHeaderLayer.frame = CGRect(x: horizontalInset + nameWidth + modifiedWidth, y: 7, width: sizeWidth, height: 16)

            rowsClipLayer.frame = CGRect(x: 0, y: 0, width: width, height: headerY)
            statusLayer.frame = CGRect(x: horizontalInset, y: max(headerY - 30, 0), width: contentWidth, height: 18)
            clampScrollOffset()
            updateFavoritesBar()
            updateBreadcrumbBar()
            updateRows(rebuild: true)
        }
        notifyAccessibilityLayoutChanged()
    }

    private func scheduleResizeLayoutUpdate() {
        guard !resizeLayoutUpdateScheduled else { return }
        resizeLayoutUpdateScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.resizeLayoutUpdateScheduled = false
            self.updateLayout()
        }
    }

    private func updateColors() {
        appearance.performAsCurrentDrawingAppearance {
            withoutImplicitAnimations {
                rootLayer.backgroundColor = NSColor.windowBackgroundColor.cgColor
                favoritesBarLayer.backgroundColor = NSColor.controlBackgroundColor.cgColor
                breadcrumbBarLayer.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.92).cgColor
                headerLayer.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.92).cgColor
                nameHeaderLayer.foregroundColor = NSColor.secondaryLabelColor.cgColor
                modifiedHeaderLayer.foregroundColor = NSColor.secondaryLabelColor.cgColor
                sizeHeaderLayer.foregroundColor = NSColor.secondaryLabelColor.cgColor
                statusLayer.foregroundColor = NSColor.secondaryLabelColor.cgColor
                rowsScrollbarController?.updateAppearance(appearance)
                updateFavoritesBar()
                updateBreadcrumbBar()
                updateRows(rebuild: true)
            }
        }
    }

    private func updateFavoritesBar() {
        withoutImplicitAnimations {
            favoritesBarLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            favoriteFrames.removeAll()

            appearance.performAsCurrentDrawingAppearance {
                if isDraggingFolderToFavorites {
                    let dropLayer = CALayer()
                    dropLayer.frame = favoritesBarLayer.bounds.insetBy(dx: horizontalInset - 4, dy: 4)
                    dropLayer.cornerRadius = 7
                    dropLayer.backgroundColor = NSColor.controlAccentColor.withAlphaComponent(0.14).cgColor
                    favoritesBarLayer.addSublayer(dropLayer)
                }

                var x = horizontalInset
                x = addFavoriteLayer(title: "Home",
                                     path: homePath ?? "~",
                                     x: x)
                for favorite in favoriteLocations {
                    guard x < favoritesBarLayer.bounds.width - horizontalInset else { break }
                    x = addFavoriteLayer(title: favorite.title,
                                         path: favorite.path,
                                         x: x)
                }
            }
        }
    }

    private func addFavoriteLayer(title: String,
                                  path: String,
                                  x: CGFloat) -> CGFloat {
        let availableWidth = max(favoritesBarLayer.bounds.width - x - horizontalInset, 0)
        guard availableWidth >= 44 else { return favoritesBarLayer.bounds.width }

        let textLayerWidth = min(textWidth(title, fontSize: 13, weight: .medium), 126)
        let width = min(max(textLayerWidth + 42, 80), availableWidth)
        let frame = CGRect(x: x, y: 6, width: width, height: 24)
        favoriteFrames.append((frame, path))

        let itemLayer = CALayer()
        itemLayer.frame = frame
        itemLayer.cornerRadius = 6
        if path == currentPath {
            itemLayer.backgroundColor = NSColor.selectedContentBackgroundColor.withAlphaComponent(0.24).cgColor
        }

        let iconLayer = CALayer()
        iconLayer.frame = CGRect(x: 8, y: 4, width: 16, height: 16)
        iconLayer.contentsGravity = .resizeAspect
        iconLayer.contentsScale = 2
        iconLayer.contents = folderIconCGImage(size: CGSize(width: 16, height: 16))
        itemLayer.addSublayer(iconLayer)

        let textLayer = makeTextLayer(size: 13, weight: .medium)
        textLayer.string = title
        textLayer.frame = CGRect(x: 30, y: 4, width: max(width - 38, 1), height: 17)
        itemLayer.addSublayer(textLayer)

        favoritesBarLayer.addSublayer(itemLayer)
        return x + width + 8
    }

    private func updateBreadcrumbBar() {
        withoutImplicitAnimations {
            breadcrumbBarLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            breadcrumbSegmentFrames.removeAll()

            appearance.performAsCurrentDrawingAppearance {
                var x = horizontalInset
                let segments = breadcrumbSegments()
                for (index, segment) in segments.enumerated() {
                    if index > 0 {
                        let separatorLayer = makeTextLayer(size: 13, weight: .regular)
                        separatorLayer.string = ">"
                        separatorLayer.foregroundColor = NSColor.tertiaryLabelColor.cgColor
                        separatorLayer.frame = CGRect(x: x, y: 9, width: 14, height: 16)
                        breadcrumbBarLayer.addSublayer(separatorLayer)
                        x += 18
                    }

                    let width = min(max(textWidth(segment.title, fontSize: 13, weight: .medium) + 18, 28),
                                    max(breadcrumbBarLayer.bounds.width - x - horizontalInset, 28))
                    let frame = CGRect(x: x, y: 5, width: width, height: 24)
                    breadcrumbSegmentFrames.append((frame, segment.path))

                    let segmentLayer = CALayer()
                    segmentLayer.frame = frame
                    segmentLayer.cornerRadius = 6
                    if segment.path == currentPath {
                        segmentLayer.backgroundColor = NSColor.selectedContentBackgroundColor.withAlphaComponent(0.18).cgColor
                    }

                    let textLayer = makeTextLayer(size: 13, weight: .medium)
                    textLayer.string = segment.title
                    textLayer.frame = CGRect(x: 9, y: 4, width: max(width - 18, 1), height: 17)
                    segmentLayer.addSublayer(textLayer)
                    breadcrumbBarLayer.addSublayer(segmentLayer)

                    x += width
                    if x >= breadcrumbBarLayer.bounds.width - horizontalInset {
                        break
                    }
                }
            }
        }
    }

    private func breadcrumbSegments() -> [BreadcrumbSegment] {
        var segments = [BreadcrumbSegment(title: "/", path: "/")]
        var accumulatedPath = ""
        for component in currentPath.split(separator: "/", omittingEmptySubsequences: true) {
            accumulatedPath += "/" + component
            segments.append(BreadcrumbSegment(title: String(component), path: accumulatedPath))
        }
        return segments
    }

    private static func inferHomePath(from path: String) -> String? {
        let components = path.split(separator: "/", omittingEmptySubsequences: true)
        guard components.count >= 2 else { return nil }

        if components[0] == "home" || components[0] == "Users" {
            return "/\(components[0])/\(components[1])"
        }

        return nil
    }

    private func registerRootLayerIfNeeded() {
        guard !hasRegisteredLayer, let registerLayer = appConnection.registerLayer else { return }
        registerLayer(rootLayer)
        hasRegisteredLayer = true
        notifyAccessibilityLayoutChanged()
    }

    private func pathFromURL(_ urlString: String?) -> String? {
        guard let urlString,
              let components = URLComponents(string: urlString),
              let path = components.queryItems?.first(where: { $0.name == "path" })?.value,
              !path.isEmpty else {
            return nil
        }
        return path
    }

    private func urlForPath(_ path: String) -> URL? {
        guard let url = outerframeHost.pluginURL(),
              var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }

        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "path" }
        queryItems.append(URLQueryItem(name: "path", value: path))
        components.queryItems = queryItems
        return components.url
    }

    private func openDirectory(path: String) {
        if let url = urlForPath(path) {
            outerframeHost.pushHistoryEntry(url: url)
        }
        fetchFiles(path: path, replaceHistoryEntryAfterLoad: path == "~" || path.hasPrefix("~/"))
    }

    private func fetchFiles(path: String, replaceHistoryEntryAfterLoad: Bool = false) {
        guard !isLoading, let filesEndpoint else { return }
        isLoading = true
        shouldReplaceHistoryEntryAfterLoad = replaceHistoryEntryAfterLoad
        statusLayer.string = "Loading..."

        guard let request = Self.binaryPathRequest(url: filesEndpoint,
                                                   magic: FilePathRequestBinaryFormat.magic,
                                                   path: path) else {
            isLoading = false
            shouldReplaceHistoryEntryAfterLoad = false
            statusLayer.string = "Could not build file request"
            return
        }

        urlSession?.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isLoading = false
                if let error {
                    self.shouldReplaceHistoryEntryAfterLoad = false
                    self.statusLayer.string = error.localizedDescription
                    self.notifyAccessibilityLayoutChanged()
                    return
                }
                guard let data else {
                    self.shouldReplaceHistoryEntryAfterLoad = false
                    self.statusLayer.string = "No response"
                    self.notifyAccessibilityLayoutChanged()
                    return
                }
                do {
                    let response = try Self.decodeFileList(data)
                    self.currentPath = response.path
                    if self.homePath == nil {
                        self.homePath = Self.inferHomePath(from: response.path) ?? (path == "~" ? response.path : nil)
                    }
                    self.parentPath = response.parent
                    self.entries = response.entries
                    self.selectedIndex = nil
                    self.resetTypeahead()
                    self.dragCandidateIndex = nil
                    self.dragStartPoint = nil
                    self.dragStartedForSelectionIndex = nil
                    self.isDraggingFolderToFavorites = false
                    self.scrollOffset = 0
                    self.statusLayer.string = response.entries.isEmpty ? "Empty folder" : ""
                    self.clampScrollOffset()
                    self.updateFavoritesBar()
                    self.updateBreadcrumbBar()
                    self.updateRows()
                    self.updatePasteboardCapabilities()
                    if self.shouldReplaceHistoryEntryAfterLoad,
                       let url = self.urlForPath(response.path) {
                        self.outerframeHost.replaceHistoryEntry(url: url)
                    }
                    self.shouldReplaceHistoryEntryAfterLoad = false
                } catch {
                    self.statusLayer.string = "Could not read file list"
                    self.shouldReplaceHistoryEntryAfterLoad = false
                    self.notifyAccessibilityLayoutChanged()
                }
            }
        }.resume()
    }

    private func updateRows(rebuild: Bool = true) {
        updateVisibleRows(rebuild: rebuild, notifyAccessibility: true)
    }

    fileprivate func setRowsScroll(_ value: CGFloat) {
        let maxOffset = max(CGFloat(entries.count) * rowHeight - rowsClipLayer.bounds.height, 0)
        let clamped = min(max(value, 0), maxOffset)
        guard abs(clamped - scrollOffset) > 0.001 else {
            updateRowsScrollbarLayout()
            return
        }
        scrollOffset = clamped
        updateVisibleRows(rebuild: false, notifyAccessibility: false)
    }

    private func updateVisibleRows(rebuild: Bool, notifyAccessibility: Bool) {
        withoutImplicitAnimations {
            if rowsContentLayer.superlayer !== rowsClipLayer {
                rowsClipLayer.addSublayer(rowsContentLayer)
            }
            if rebuild {
                recycleAllRowLayers()
            }

            let viewportHeight = max(rowsClipLayer.bounds.height, 0)
            let viewportWidth = max(rowsClipLayer.bounds.width, 1)
            let contentHeight = CGFloat(entries.count) * rowHeight
            scrollOffset = min(max(scrollOffset, 0), max(contentHeight - viewportHeight, 0))
            rowsContentLayer.frame = CGRect(x: 0,
                                            y: viewportHeight - contentHeight + scrollOffset,
                                            width: viewportWidth,
                                            height: max(contentHeight, 0))

            guard !entries.isEmpty, viewportHeight > 0 else {
                recycleAllRowLayers()
                rowsContentLayer.frame = CGRect(origin: .zero, size: CGSize(width: viewportWidth, height: 0))
                updateRowsScrollbarLayout()
                if notifyAccessibility {
                    notifyAccessibilityLayoutChanged()
                }
                return
            }

            appearance.performAsCurrentDrawingAppearance {
                let visibleStart = max(Int(floor(scrollOffset / rowHeight)), 0)
                let visibleEnd = min(entries.count, visibleStart + Int(ceil(viewportHeight / rowHeight)) + 2)
                let visibleRange = visibleStart..<visibleEnd

                let staleIndices = visibleRowLayers.keys.filter { !visibleRange.contains($0) }
                for index in staleIndices {
                    if let layer = visibleRowLayers[index] {
                        recycleRowLayer(layer)
                    }
                    visibleRowLayers.removeValue(forKey: index)
                }

                let contentWidth = max(viewportWidth - horizontalInset * 2, 1)
                let nameWidth = floor(contentWidth * nameColumnWidth)
                let modifiedWidth = floor(contentWidth * modifiedColumnWidth)
                let sizeWidth = max(contentWidth - nameWidth - modifiedWidth, 1)
                let rowColors = alternatingRowColors()
                let selectedBackgroundColor = NSColor.controlAccentColor.cgColor
                let selectedTextColor = NSColor.white.cgColor
                let bodyTextColor = NSColor.labelColor.cgColor
                let secondaryTextColor = NSColor.secondaryLabelColor.cgColor

                for index in visibleRange {
                    let entry = entries[index]
                    let isSelected = selectedIndex == index
                    let top = contentHeight - CGFloat(index + 1) * rowHeight
                    let frame = CGRect(x: 0, y: top, width: viewportWidth, height: rowHeight)
                    let rowLayer: CALayer
                    let needsConfigure: Bool
                    if let existing = visibleRowLayers[index] {
                        rowLayer = existing
                        needsConfigure = rebuild || existing.frame.size != frame.size || existing.frame.origin != frame.origin
                    } else if let reusable = reusableRowLayers.popLast() {
                        rowLayer = reusable
                        rowsContentLayer.addSublayer(rowLayer)
                        visibleRowLayers[index] = rowLayer
                        needsConfigure = true
                    } else {
                        rowLayer = makeRowLayer()
                        rowsContentLayer.addSublayer(rowLayer)
                        visibleRowLayers[index] = rowLayer
                        needsConfigure = true
                    }
                    if needsConfigure {
                        configureRowLayer(rowLayer,
                                          entry: entry,
                                          index: index,
                                          frame: frame,
                                          isSelected: isSelected,
                                          rowColors: rowColors,
                                          selectedBackgroundColor: selectedBackgroundColor,
                                          selectedTextColor: selectedTextColor,
                                          bodyTextColor: bodyTextColor,
                                          secondaryTextColor: secondaryTextColor,
                                          nameWidth: nameWidth,
                                          modifiedWidth: modifiedWidth,
                                          sizeWidth: sizeWidth)
                    }
                }
            }
        }
        updateRowsScrollbarLayout()
        if notifyAccessibility {
            notifyAccessibilityLayoutChanged()
        }
    }

    private func makeRowLayer() -> CALayer {
        let rowLayer = CALayer()

        let iconLayer = CALayer()
        iconLayer.contentsGravity = .resizeAspect
        iconLayer.contentsScale = 2
        rowLayer.addSublayer(iconLayer)

        rowLayer.addSublayer(makeTextLayer(size: 13, weight: .regular))
        rowLayer.addSublayer(makeTextLayer(size: 12, weight: .regular))
        rowLayer.addSublayer(makeTextLayer(size: 12, weight: .regular, alignment: .right))
        return rowLayer
    }

    private func configureRowLayer(_ rowLayer: CALayer,
                                   entry: FileEntry,
                                   index: Int,
                                   frame: CGRect,
                                   isSelected: Bool,
                                   rowColors: (even: CGColor, odd: CGColor),
                                   selectedBackgroundColor: CGColor,
                                   selectedTextColor: CGColor,
                                   bodyTextColor: CGColor,
                                   secondaryTextColor: CGColor,
                                   nameWidth: CGFloat,
                                   modifiedWidth: CGFloat,
                                   sizeWidth: CGFloat) {
        rowLayer.frame = frame
        if isSelected {
            rowLayer.backgroundColor = selectedBackgroundColor
        } else if index.isMultiple(of: 2) {
            rowLayer.backgroundColor = rowColors.even
        } else {
            rowLayer.backgroundColor = rowColors.odd
        }

        if rowLayer.sublayers?.count != 4 {
            rowLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            let iconLayer = CALayer()
            iconLayer.contentsGravity = .resizeAspect
            iconLayer.contentsScale = 2
            rowLayer.addSublayer(iconLayer)
            rowLayer.addSublayer(makeTextLayer(size: 13, weight: .regular))
            rowLayer.addSublayer(makeTextLayer(size: 12, weight: .regular))
            rowLayer.addSublayer(makeTextLayer(size: 12, weight: .regular, alignment: .right))
        }

        let iconLayer = rowLayer.sublayers?[0]
        iconLayer?.frame = CGRect(x: horizontalInset, y: 5, width: 16, height: 16)
        iconLayer?.contents = rowIconCGImage(for: entry, size: CGSize(width: 16, height: 16))

        if let nameLayer = rowLayer.sublayers?[1] as? CATextLayer {
            nameLayer.string = entry.name
            nameLayer.foregroundColor = isSelected ? selectedTextColor : bodyTextColor
            nameLayer.frame = CGRect(x: horizontalInset + 24, y: 5, width: max(nameWidth - 24, 1), height: 17)
        }

        if let modifiedLayer = rowLayer.sublayers?[2] as? CATextLayer {
            modifiedLayer.string = formatModified(entry.modified)
            modifiedLayer.foregroundColor = isSelected ? selectedTextColor : secondaryTextColor
            modifiedLayer.frame = CGRect(x: horizontalInset + nameWidth, y: 5, width: modifiedWidth, height: 17)
        }

        if let sizeLayer = rowLayer.sublayers?[3] as? CATextLayer {
            sizeLayer.string = entry.isDirectory ? "--" : formatByteCount(entry.size)
            sizeLayer.foregroundColor = isSelected ? selectedTextColor : secondaryTextColor
            sizeLayer.frame = CGRect(x: horizontalInset + nameWidth + modifiedWidth, y: 5, width: sizeWidth, height: 17)
        }
    }

    private func recycleRowLayer(_ layer: CALayer) {
        layer.removeFromSuperlayer()
        reusableRowLayers.append(layer)
    }

    private func recycleAllRowLayers() {
        rowsContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        visibleRowLayers.removeAll()
        reusableRowLayers.removeAll()
    }

    private func rowsScrollbarMetrics() -> ScrollbarController<FilesRowsScrollbarDelegate>.Metrics {
        ScrollbarController.Metrics(viewportSize: rowsClipLayer.bounds.size,
                                    contentHeight: CGFloat(entries.count) * rowHeight,
                                    scrollOffset: scrollOffset)
    }

    private func updateRowsScrollbarLayout() {
        rowsScrollbarController?.updateLayout(metrics: rowsScrollbarMetrics())
    }

    private func handleMouseDown(at point: CGPoint, clickCount: Int) {
        dragCandidateIndex = nil
        dragStartPoint = nil
        dragStartedForSelectionIndex = nil
        isDraggingFolderToFavorites = false

        if let favoritePath = favoritePath(at: point) {
            selectedIndex = nil
            resetTypeahead()
            updateRows()
            updatePasteboardCapabilities()
            updateFavoritesBar()
            openDirectory(path: favoritePath)
            return
        }

        if let breadcrumbPath = breadcrumbPath(at: point) {
            selectedIndex = nil
            resetTypeahead()
            updateRows()
            updatePasteboardCapabilities()
            updateFavoritesBar()
            openDirectory(path: breadcrumbPath)
            return
        }

        if rowsClipLayer.bounds.contains(rowsClipLayer.convert(point, from: rootLayer)),
           rowsScrollbarController?.handleMouseDown(at: rootLayer.convert(point, to: rowsClipLayer)) == true {
            return
        }

        let index = rowIndex(at: point)
        guard index >= 0, index < entries.count else {
            selectedIndex = nil
            resetTypeahead()
            updateRows()
            updatePasteboardCapabilities()
            updateFavoritesBar()
            return
        }

        selectedIndex = index
        resetTypeahead()
        dragCandidateIndex = index
        dragStartPoint = point
        updateRows()
        updatePasteboardCapabilities()
        updateFavoritesBar()

        if clickCount >= 2 {
            dragCandidateIndex = nil
            dragStartPoint = nil
            let entry = entries[index]
            if entry.isDirectory {
                openDirectory(path: entry.path)
            } else {
                openFileWithDefaultOpener(entry, openInNewTab: true)
            }
        }
    }

    private func handleMouseDragged(to point: CGPoint, modifierFlags _: NSEvent.ModifierFlags) {
        if rowsScrollbarController?.handleMouseDragged(to: rootLayer.convert(point, to: rowsClipLayer)) == true {
            return
        }

        guard let selectedIndex,
              dragCandidateIndex == selectedIndex,
              dragStartedForSelectionIndex != selectedIndex,
              entries.indices.contains(selectedIndex) else {
            return
        }

        let entry = entries[selectedIndex]
        if entry.isDirectory {
            if favoritesBarContains(point) {
                if !isDraggingFolderToFavorites {
                    isDraggingFolderToFavorites = true
                    updateFavoritesBar()
                }
                return
            }

            if isDraggingFolderToFavorites {
                isDraggingFolderToFavorites = false
                updateFavoritesBar()
            }

            if let dragStartPoint, point.y > dragStartPoint.y + 4 {
                return
            }
        }

        dragStartedForSelectionIndex = selectedIndex
        beginDraggingFilePromise(for: entry, at: selectedIndex)
    }

    private func handleMouseUp(at point: CGPoint) {
        _ = rowsScrollbarController?.handleMouseUp(at: rootLayer.convert(point, to: rowsClipLayer))
        defer {
            dragCandidateIndex = nil
            dragStartPoint = nil
            dragStartedForSelectionIndex = nil
            if isDraggingFolderToFavorites {
                isDraggingFolderToFavorites = false
                updateFavoritesBar()
            }
        }

        guard let selectedIndex,
              dragCandidateIndex == selectedIndex,
              entries.indices.contains(selectedIndex) else {
            return
        }

        let entry = entries[selectedIndex]
        guard entry.isDirectory, favoritesBarContains(point) else { return }
        addFavorite(entry)
    }

    private func handleKeyDown(keyCode: UInt16, characters: String?) {
        switch keyCode {
        case 126:
            dragCandidateIndex = nil
            dragStartPoint = nil
            resetTypeahead()
            moveSelection(delta: -1)
        case 125:
            dragCandidateIndex = nil
            dragStartPoint = nil
            resetTypeahead()
            moveSelection(delta: 1)
        case 36, 76:
            if let selectedIndex, entries[selectedIndex].isDirectory {
                openDirectory(path: entries[selectedIndex].path)
            }
        case 51:
            if let parentPath {
                openDirectory(path: parentPath)
            }
        default:
            if let characters,
               let text = typeaheadText(from: characters) {
                handleTypeahead(text)
            }
            break
        }
    }

    private func moveSelection(delta: Int) {
        guard !entries.isEmpty else { return }
        let nextIndex = min(max((selectedIndex ?? (delta > 0 ? -1 : entries.count)) + delta, 0), entries.count - 1)
        selectedIndex = nextIndex
        let rowTop = CGFloat(nextIndex) * rowHeight
        let viewportHeight = rowsClipLayer.bounds.height
        if rowTop < scrollOffset {
            scrollOffset = rowTop
        } else if rowTop + rowHeight > scrollOffset + viewportHeight {
            scrollOffset = rowTop + rowHeight - viewportHeight
        }
        clampScrollOffset()
        updateRows()
        updatePasteboardCapabilities()
    }

    private func selectIndex(_ index: Int) {
        guard entries.indices.contains(index) else { return }
        selectedIndex = index
        ensureSelectionVisible()
        updateRows()
        updatePasteboardCapabilities()
    }

    private func ensureSelectionVisible() {
        guard let selectedIndex else { return }
        let rowTop = CGFloat(selectedIndex) * rowHeight
        let viewportHeight = rowsClipLayer.bounds.height
        if rowTop < scrollOffset {
            scrollOffset = rowTop
        } else if rowTop + rowHeight > scrollOffset + viewportHeight {
            scrollOffset = rowTop + rowHeight - viewportHeight
        }
        clampScrollOffset()
    }

    private func resetTypeahead() {
        typeaheadPrefix = ""
        typeaheadLastUpdated = nil
    }

    private func handleTypeahead(_ text: String) {
        guard !entries.isEmpty else { return }
        let now = Date()
        if let last = typeaheadLastUpdated,
           now.timeIntervalSince(last) > 1.0 {
            typeaheadPrefix = ""
        }
        typeaheadLastUpdated = now
        typeaheadPrefix += text.lowercased()
        if selectEntry(matchingPrefix: typeaheadPrefix) {
            return
        }
        typeaheadPrefix = text.lowercased()
        _ = selectEntry(matchingPrefix: typeaheadPrefix)
    }

    private func selectEntry(matchingPrefix prefix: String) -> Bool {
        guard !prefix.isEmpty else { return false }
        let start = (selectedIndex ?? -1) + 1
        for offset in 0..<entries.count {
            let index = (start + offset) % entries.count
            if entries[index].name.lowercased().hasPrefix(prefix) {
                selectIndex(index)
                return true
            }
        }
        return false
    }

    private func typeaheadText(from characters: String) -> String? {
        guard !characters.isEmpty,
              characters.unicodeScalars.allSatisfy({
                  !CharacterSet.controlCharacters.contains($0) &&
                  !CharacterSet.newlines.contains($0)
              }) else {
            return nil
        }
        return characters
    }

    private func clampScrollOffset() {
        let maxOffset = max(CGFloat(entries.count) * rowHeight - rowsClipLayer.bounds.height, 0)
        scrollOffset = min(max(scrollOffset, 0), maxOffset)
    }

    private func updatePasteboardCapabilities() {
        outerframeHost.setAcceptedPasteboardPasteTypes([
            NSPasteboard.PasteboardType.fileURL.rawValue,
            Self.droppedFileAccessPasteboardTypeIdentifier
        ])
        outerframeHost.setPasteboardDropBehaviorUniform([
            NSPasteboard.PasteboardType.fileURL.rawValue,
            Self.droppedFileAccessPasteboardTypeIdentifier,
            NSPasteboard.PasteboardType.string.rawValue
        ])
    }

    private func enabledEditCommands(in requestedCommands: OuterframeEditCommandSet) -> OuterframeEditCommandSet {
        var enabledCommands: OuterframeEditCommandSet = []
        if requestedCommands.contains(.copy), selectedFileEntryForCopy() != nil {
            enabledCommands.insert(.copy)
        }
        if requestedCommands.contains(.paste) {
            enabledCommands.insert(.paste)
        }
        return enabledCommands
    }

    private func selectedFileEntryForCopy() -> FileEntry? {
        guard let selectedIndex,
              entries.indices.contains(selectedIndex) else { return nil }
        let entry = entries[selectedIndex]
        return entry.isDirectory ? nil : entry
    }

    private func handleSelectionToPasteboardCopyRequest(requestID: UUID) {
        guard let entry = selectedFileEntryForCopy(),
              let downloadEndpoint,
              let urlSession,
              let stagingDirectoryURL = outerframeHost.stagedFileDirectoryURL else {
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID, items: [])
            return
        }

        var components = URLComponents(url: downloadEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "path", value: entry.path)]
        guard let downloadURL = components?.url else {
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID, items: [])
            return
        }

        let copyID = UUID()
        let targetDirectoryURL = stagingDirectoryURL
            .appendingPathComponent("files-copy-\(copyID.uuidString)", isDirectory: true)
        let targetURL = targetDirectoryURL
            .appendingPathComponent(Self.safeStagedFileName(entry.name), isDirectory: false)

        statusLayer.string = "Copying \(entry.name)..."
        Task { [weak self, urlSession, downloadURL, targetDirectoryURL, targetURL, requestID] in
            do {
                try await Self.writeDownload(from: downloadURL,
                                             using: urlSession,
                                             to: targetURL,
                                             creating: targetDirectoryURL)
                let item = OuterframeContentPasteboardItem(representations: [
                    OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.fileURL.rawValue,
                                                              data: Data(targetURL.absoluteString.utf8))
                ])
                guard let self else { return }
                self.statusLayer.string = ""
                self.outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                                       items: [item])
            } catch {
                try? FileManager.default.removeItem(at: targetDirectoryURL)
                guard let self else { return }
                self.statusLayer.string = "Copy failed"
                self.outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                                       items: [])
                print("Files copy: failed to stage \(targetURL.path): \(error)")
            }
        }
    }

    private func contextMenuSeparator(id: String) -> OuterframeContextMenuItem {
        OuterframeContextMenuItem(id: id,
                                  title: "",
                                  kind: .separator,
                                  isEnabled: false)
    }

    private func handleRightMouseDown(at point: CGPoint) {
        let index = rowIndex(at: point)
        if entries.indices.contains(index), entries[index].isDirectory {
            let entry = entries[index]
            selectedIndex = index
            updateRows()
            updatePasteboardCapabilities()

            let menuID = UUID()
            pendingDirectoryMenuEntries[menuID] = entry
            outerframeHost.showContextMenu(menuID: menuID,
                                           items: [
                                            OuterframeContextMenuItem(id: "open-directory",
                                                                      title: "Open",
                                                                      isEnabled: true,
                                                                      systemImageName: "folder"),
                                            OuterframeContextMenuItem(id: "add-to-favorites",
                                                                      title: "Add to Favorites",
                                                                      isEnabled: !isFavoritePath(entry.path),
                                                                      systemImageName: "star"),
                                            contextMenuSeparator(id: "directory-separator"),
                                            OuterframeContextMenuItem(id: "paste",
                                                                      title: "Paste",
                                                                      action: .standardPaste,
                                                                      isEnabled: true,
                                                                      systemImageName: "clipboard")
                                           ],
                                           at: point)
            return
        }

        if entries.indices.contains(index) {
            selectedIndex = index
            updateRows()
            updatePasteboardCapabilities()
            let entry = entries[index]
            fetchOpeners(for: entry) { [weak self] result in
                guard let self else { return }
                let menuID = UUID()
                var items: [OuterframeContextMenuItem] = []
                switch result {
                case .success(let openers):
                    let openerActions = self.openMenuActions(for: entry, openers: openers)
                    if !openerActions.isEmpty {
                        self.pendingOpenMenuEntries[menuID] = (entry, openerActions.map(\.opener))
                        for (index, action) in openerActions.enumerated() {
                            items.append(OuterframeContextMenuItem(id: "open-\(index)",
                                                                   title: action.title,
                                                                   isEnabled: true,
                                                                   systemImageName: "arrow.up.forward"))
                        }
                        items.append(self.contextMenuSeparator(id: "copy-separator"))
                    }
                case .failure(let message):
                    items.append(OuterframeContextMenuItem(id: "openers-error",
                                                           title: "Could not load openers: \(message)",
                                                           isEnabled: false,
                                                           systemImageName: "exclamationmark.triangle"))
                    items.append(self.contextMenuSeparator(id: "copy-separator"))
                }
                items.append(OuterframeContextMenuItem(id: "copy",
                                                       title: "Copy",
                                                       action: .standardCopy,
                                                       isEnabled: true,
                                                       systemImageName: "doc.on.doc"))
                self.outerframeHost.showContextMenu(menuID: menuID,
                                                    items: items,
                                                    at: point)
            }
            return
        }

        outerframeHost.showContextMenu(menuID: UUID(),
                                       items: [
                                        OuterframeContextMenuItem(id: "paste",
                                                                  title: "Paste",
                                                                  action: .standardPaste,
                                                                  isEnabled: true,
                                                                  systemImageName: "clipboard")
                                       ],
                                       at: point)
    }

    private func openerBaseTitle(_ opener: FileOpener) -> String {
        let displayName = opener.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        if !displayName.isEmpty {
            return displayName
        }
        return opener.serviceID.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func duplicateOpenerBaseTitles(_ openers: [FileOpener]) -> Set<String> {
        var counts: [String: Int] = [:]
        for opener in openers {
            counts[openerBaseTitle(opener), default: 0] += 1
        }
        return Set(counts.compactMap { title, count in count > 1 ? title : nil })
    }

    private func qualifiedOpenerTitle(for opener: FileOpener, forceOwner: Bool) -> String {
        let baseTitle = openerBaseTitle(opener)
        let ownerName = opener.ownerName.trimmingCharacters(in: .whitespacesAndNewlines)
        if forceOwner, !ownerName.isEmpty {
            return "\(ownerName) / \(baseTitle)"
        }
        return baseTitle
    }

    private func isRootOpener(_ opener: FileOpener) -> Bool {
        opener.ownerName.trimmingCharacters(in: .whitespacesAndNewlines) == "root"
    }

    private func isSameOpener(_ lhs: FileOpener, _ rhs: FileOpener) -> Bool {
        lhs.serviceID == rhs.serviceID &&
        lhs.socketPath == rhs.socketPath &&
        lhs.url == rhs.url &&
        lhs.ownerName == rhs.ownerName
    }

    private func openMenuActions(for entry: FileEntry, openers: [FileOpener]) -> [FileOpenMenuAction] {
        guard !openers.isEmpty else { return [] }

        var groupedOpeners: [String: [FileOpener]] = [:]
        var groupOrder: [String] = []
        for opener in openers {
            let baseTitle = openerBaseTitle(opener)
            if groupedOpeners[baseTitle] == nil {
                groupOrder.append(baseTitle)
            }
            groupedOpeners[baseTitle, default: []].append(opener)
        }

        let duplicateBaseTitles = duplicateOpenerBaseTitles(openers)
        var actions: [FileOpenMenuAction] = []
        for baseTitle in groupOrder {
            guard let group = groupedOpeners[baseTitle] else { continue }
            let rootOpener = group.first(where: { isRootOpener($0) })
            let userOpener = group.first(where: { !isRootOpener($0) })

            if let rootOpener, let userOpener {
                if entry.userCanModify {
                    actions.append(FileOpenMenuAction(title: "Open in \(baseTitle)",
                                                      opener: userOpener))
                } else if entry.userCanView {
                    actions.append(FileOpenMenuAction(title: "View in \(baseTitle)",
                                                      opener: userOpener))
                    actions.append(FileOpenMenuAction(title: "Edit in \(baseTitle) (root)",
                                                      opener: rootOpener))
                } else {
                    actions.append(FileOpenMenuAction(title: "Open in \(baseTitle)",
                                                      opener: rootOpener))
                }

                for opener in group where !isSameOpener(opener, rootOpener) && !isSameOpener(opener, userOpener) {
                    let title = qualifiedOpenerTitle(for: opener, forceOwner: duplicateBaseTitles.contains(baseTitle))
                    actions.append(FileOpenMenuAction(title: "Open in \(title)",
                                                      opener: opener))
                }
            } else {
                for opener in group {
                    let title = qualifiedOpenerTitle(for: opener, forceOwner: duplicateBaseTitles.contains(baseTitle))
                    actions.append(FileOpenMenuAction(title: "Open in \(title)",
                                                      opener: opener))
                }
            }
        }
        return actions
    }

    private func handleContextMenuItemSelected(menuID: UUID, itemID: String) {
        if let pending = pendingOpenMenuEntries.removeValue(forKey: menuID),
           itemID.hasPrefix("open-"),
           let index = Int(itemID.dropFirst("open-".count)),
           pending.openers.indices.contains(index) {
            navigateToFile(entry: pending.entry, with: pending.openers[index], openInNewTab: true)
            return
        }

        guard let entry = pendingDirectoryMenuEntries.removeValue(forKey: menuID) else {
            return
        }
        if itemID == "open-directory" {
            openDirectory(path: entry.path)
        } else if itemID == "add-to-favorites" {
            addFavorite(entry)
        }
    }

    private func openFileWithDefaultOpener(_ entry: FileEntry, openInNewTab: Bool) {
        guard !entry.isDirectory else { return }
        statusLayer.string = ""
        fetchOpeners(for: entry) { [weak self] result in
            guard let self else { return }
            let openers: [FileOpener]
            switch result {
            case .success(let fetchedOpeners):
                openers = fetchedOpeners
            case .failure(let message):
                self.statusLayer.string = "Could not load openers: \(message)"
                return
            }
            guard let opener = self.openMenuActions(for: entry, openers: openers).first?.opener else {
                self.statusLayer.string = "No app found for \(entry.name)"
                return
            }
            self.navigateToFile(entry: entry, with: opener, openInNewTab: openInNewTab)
        }
    }

    private func navigateToFile(entry: FileEntry, with opener: FileOpener, openInNewTab: Bool) {
        guard !entry.isDirectory,
              let url = openerNavigationURL(opener) else {
            statusLayer.string = "Could not open \(entry.name)"
            return
        }
        statusLayer.string = ""
        if openInNewTab {
            outerframeHost.openNewTab(with: url, displayString: nil)
        } else {
            outerframeHost.navigate(to: url)
        }
    }

    private func fetchOpeners(for entry: FileEntry, completion: @escaping @MainActor (FileOpenersFetchResult) -> Void) {
        guard !entry.isDirectory,
              let openersEndpoint,
              let urlSession else {
            completion(.success([]))
            return
        }
        guard let request = Self.binaryPathRequest(url: openersEndpoint,
                                                   magic: FileOpenersBinaryFormat.requestMagic,
                                                   path: entry.path) else {
            completion(.failure("could not build request"))
            return
        }
        urlSession.dataTask(with: request) { data, response, error in
            let result: FileOpenersFetchResult
            if let error {
                result = .failure(error.localizedDescription)
            } else if let httpResponse = response as? HTTPURLResponse,
                      !(200..<300).contains(httpResponse.statusCode) {
                result = .failure(Self.responseErrorMessage(data: data, fallback: "HTTP \(httpResponse.statusCode)"))
            } else if let data,
                      let openers = Self.decodeFileOpeners(data) {
                result = .success(openers)
            } else {
                result = .failure("invalid response")
            }
            Task { @MainActor in
                completion(result)
            }
        }.resume()
    }

    nonisolated private static func responseErrorMessage(data: Data?, fallback: String) -> String {
        guard let data,
              let rawMessage = String(data: data, encoding: .utf8) else {
            return fallback
        }
        let message = rawMessage.trimmingCharacters(in: .whitespacesAndNewlines)
        return message.isEmpty ? fallback : message
    }

    nonisolated private static func binaryPathRequest(url: URL, magic: UInt32, path: String) -> URLRequest? {
        var payload = BinaryPayloadBuilder(referenceBaseOffset: 0)
        payload.append(uint32: magic)
        payload.append(uint32: 1)
        guard payload.append(stringReference: path),
              let data = payload.finalize() else {
            return nil
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.httpBody = data
        return request
    }

    nonisolated private static func binaryDirectoryNameRequest(url: URL,
                                                              magic: UInt32,
                                                              directory: String,
                                                              name: String) -> URLRequest? {
        var payload = BinaryPayloadBuilder(referenceBaseOffset: 0)
        payload.append(uint32: magic)
        payload.append(uint32: 1)
        guard payload.append(stringReference: directory),
              payload.append(stringReference: name),
              let data = payload.finalize() else {
            return nil
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.httpBody = data
        return request
    }

    nonisolated private static func decodeFileList(_ data: Data) throws -> FileListResponse {
        guard data.count >= FileListBinaryFormat.headerSize else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError)
        }
        var header = BinaryPayloadCursor(data)
        guard header.readUInt32() == FileListBinaryFormat.magic,
              header.readUInt32() == FileListBinaryFormat.version,
              let rowCountValue = header.readUInt32(),
              header.readUInt32() == UInt32(FileListBinaryFormat.rowSize),
              let rowsOffsetValue = header.readUInt32(),
              let variableOffsetValue = header.readUInt32(),
              let totalSizeValue = header.readUInt32() else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError)
        }
        _ = header.readUInt32()
        guard let path = header.readStringReference(),
              let parent = header.readStringReference() else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError)
        }

        let rowCount = Int(rowCountValue)
        let rowsOffset = Int(rowsOffsetValue)
        let variableOffset = Int(variableOffsetValue)
        let totalSize = Int(totalSizeValue)
        guard totalSize <= data.count,
              rowsOffset >= FileListBinaryFormat.headerSize,
              variableOffset >= rowsOffset,
              rowCount <= (variableOffset - rowsOffset) / FileListBinaryFormat.rowSize else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError)
        }

        var entries: [FileEntry] = []
        entries.reserveCapacity(rowCount)
        for index in 0..<rowCount {
            var row = BinaryPayloadCursor(data, offset: rowsOffset + index * FileListBinaryFormat.rowSize)
            guard let name = row.readStringReference(),
                  let path = row.readStringReference(),
                  let mode = row.readStringReference(),
                  let isDirectoryValue = row.readUInt32(),
                  let accessFlags = row.readUInt32(),
                  let size = row.readUInt64(),
                  let modifiedMillis = row.readUInt64() else {
                throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError)
            }
            entries.append(FileEntry(name: name,
                                     path: path,
                                     isDirectory: isDirectoryValue != 0,
                                     size: size,
                                     modified: Double(modifiedMillis) / 1000.0,
                                     mode: mode,
                                     accessFlags: accessFlags))
        }
        return FileListResponse(path: path, parent: parent.isEmpty ? nil : parent, entries: entries)
    }

    nonisolated private static func decodeFileOpeners(_ data: Data) -> [FileOpener]? {
        guard data.count >= FileOpenersBinaryFormat.headerSize else { return nil }
        var header = BinaryPayloadCursor(data)
        guard header.readUInt32() == FileOpenersBinaryFormat.magic,
              header.readUInt32() == FileOpenersBinaryFormat.version,
              let rowCountValue = header.readUInt32(),
              header.readUInt32() == UInt32(FileOpenersBinaryFormat.rowSize),
              let rowsOffsetValue = header.readUInt32(),
              let variableOffsetValue = header.readUInt32(),
              let totalSizeValue = header.readUInt32() else {
            return nil
        }

        let rowCount = Int(rowCountValue)
        let rowsOffset = Int(rowsOffsetValue)
        let variableOffset = Int(variableOffsetValue)
        let totalSize = Int(totalSizeValue)
        guard totalSize <= data.count,
              rowsOffset >= FileOpenersBinaryFormat.headerSize,
              variableOffset >= rowsOffset,
              rowCount <= (variableOffset - rowsOffset) / FileOpenersBinaryFormat.rowSize else {
            return nil
        }

        var openers: [FileOpener] = []
        openers.reserveCapacity(rowCount)
        for index in 0..<rowCount {
            var row = BinaryPayloadCursor(data, offset: rowsOffset + index * FileOpenersBinaryFormat.rowSize)
            guard let contentType = row.readStringReference(),
                  let serviceID = row.readStringReference(),
                  let displayName = row.readStringReference(),
                  let socketPath = row.readStringReference(),
                  let url = row.readStringReference(),
                  let ownerName = row.readStringReference() else {
                return nil
            }
            openers.append(FileOpener(contentType: contentType,
                                      serviceID: serviceID,
                                      displayName: displayName,
                                      socketPath: socketPath,
                                      url: url,
                                      ownerName: ownerName))
        }
        return openers
    }

    private func openerNavigationURL(_ opener: FileOpener) -> URL? {
        let socketPath = opener.socketPath.trimmingCharacters(in: .whitespacesAndNewlines)
        if !socketPath.isEmpty {
            let path = pathAndQuery(fromOpenerURL: opener.url, socketPath: socketPath)
            return URL(string: "http+unix://\(percentEncodedSocketPath(socketPath))\(path)")
        }

        let rawURL = opener.url.trimmingCharacters(in: .whitespacesAndNewlines)
        if let parsed = URL(string: rawURL), parsed.scheme != nil {
            return parsed
        }
        return URL(string: rawURL)
    }

    private func pathAndQuery(fromOpenerURL rawURL: String, socketPath: String) -> String {
        let trimmed = rawURL.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return "/" }

        if trimmed == socketPath { return "/" }
        if trimmed.hasPrefix(socketPath) {
            let suffix = String(trimmed.dropFirst(socketPath.count))
            return normalizedPathAndQuery(suffix)
        }

        if trimmed.lowercased().hasPrefix("http+unix://") {
            let prefixLength = "http+unix://".count
            let startIndex = trimmed.index(trimmed.startIndex, offsetBy: prefixLength)
            let authorityAndSuffix = String(trimmed[startIndex...])
            let suffixStart = authorityAndSuffix.firstIndex(where: { $0 == "/" || $0 == "?" || $0 == "#" }) ?? authorityAndSuffix.endIndex
            return normalizedPathAndQuery(String(authorityAndSuffix[suffixStart...]))
        }

        if let components = URLComponents(string: trimmed), components.scheme != nil {
            var path = components.path.isEmpty ? "/" : components.path
            if let query = components.query, !query.isEmpty {
                path += "?\(query)"
            }
            return normalizedPathAndQuery(path)
        }

        if trimmed.hasPrefix("/") || trimmed.hasPrefix("?") || trimmed.hasPrefix("#") {
            return normalizedPathAndQuery(trimmed)
        }
        return normalizedPathAndQuery(trimmed)
    }

    private func normalizedPathAndQuery(_ value: String) -> String {
        if value.isEmpty { return "/" }
        if value.hasPrefix("/") { return value }
        if value.hasPrefix("?") || value.hasPrefix("#") { return "/\(value)" }
        return "/\(value)"
    }

    private func percentEncodedSocketPath(_ socketPath: String) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return socketPath.addingPercentEncoding(withAllowedCharacters: allowed) ?? socketPath
    }

    private func addFavorite(_ entry: FileEntry) {
        guard entry.isDirectory, !isFavoritePath(entry.path) else { return }
        favoriteLocations.append(FavoriteLocation(title: entry.name, path: entry.path))
        updateFavoritesBar()
    }

    private func isFavoritePath(_ path: String) -> Bool {
        if let homePath, path == homePath {
            return true
        }
        if homePath == nil, path == "~" {
            return true
        }
        return favoriteLocations.contains { $0.path == path }
    }

    private func beginDraggingFilePromise(for entry: FileEntry, at index: Int) {
        guard outerframeHost.stagedFileDirectoryURL != nil else {
            dragStartedForSelectionIndex = nil
            statusLayer.string = "Could not prepare drag"
            print("Files drag: missing staging directory for \(entry.path)")
            return
        }

        let promiseID = UUID()
        filePromiseEntries[promiseID] = entry
        let dragPreview = dragPreview(for: entry)
        guard let pasteboardItem = outerframeHost.filePromisePasteboardItem(promiseID: promiseID,
                                                                            name: entry.name,
                                                                            fileSize: entry.isDirectory ? nil : entry.size,
                                                                            fileType: fileTypeIdentifier(for: entry)) else {
            filePromiseEntries.removeValue(forKey: promiseID)
            dragStartedForSelectionIndex = nil
            statusLayer.string = "Could not prepare drag"
            return
        }
        outerframeHost.beginDraggingPasteboardItem(pasteboardItem,
                                                   operationMask: .copy,
                                                   previewPNGData: dragPreview?.pngData,
                                                   previewSize: dragPreview?.size,
                                                   previewFrameOrigin: dragPreviewOrigin(forRowAt: index))
    }

    private func handleFilePromiseWriteRequest(requestID: UUID, promiseID: UUID) {
        guard let entry = filePromiseEntries[promiseID] else {
            outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                       promiseID: promiseID,
                                                       errorMessage: "Unknown file promise.")
            return
        }
        stagePromisedFile(entry: entry, promiseID: promiseID, requestID: requestID)
    }

    private func stagePromisedFile(entry: FileEntry, promiseID: UUID, requestID: UUID) {
        guard let downloadEndpoint,
              let urlSession,
              let stagingDirectoryURL = outerframeHost.stagedFileDirectoryURL else {
            outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                       promiseID: promiseID,
                                                       errorMessage: "Missing download endpoint, URLSession, or staging directory.")
            return
        }

        var components = URLComponents(url: downloadEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "path", value: entry.path)]
        guard let downloadURL = components?.url else {
            outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                       promiseID: promiseID,
                                                       errorMessage: "Could not build download URL.")
            print("Files drag: failed to build download URL for \(entry.path)")
            return
        }

        let targetDirectoryURL = stagingDirectoryURL
            .appendingPathComponent("files-promise-\(promiseID.uuidString)", isDirectory: true)
        let targetURL = targetDirectoryURL
            .appendingPathComponent(Self.safeStagedFileName(entry.name), isDirectory: entry.isDirectory)

        statusLayer.string = "Preparing \(entry.name)..."
        Task { [weak self, urlSession, downloadURL, targetDirectoryURL, targetURL, entry, promiseID, requestID] in
            do {
                if entry.isDirectory {
                    guard let filesEndpoint = self?.filesEndpoint else {
                        throw NSError(domain: NSCocoaErrorDomain,
                                      code: NSFileReadUnknownError,
                                      userInfo: [NSLocalizedDescriptionKey: "Missing files endpoint."])
                    }
                    try await Self.stageRemoteDirectory(path: entry.path,
                                                        to: targetURL,
                                                        creating: targetDirectoryURL,
                                                        filesEndpoint: filesEndpoint,
                                                        downloadEndpoint: downloadEndpoint,
                                                        using: urlSession)
                } else {
                    try await Self.writeDownload(from: downloadURL,
                                                 using: urlSession,
                                                 to: targetURL,
                                                 creating: targetDirectoryURL)
                }

                guard let self else { return }
                self.statusLayer.string = ""
                self.filePromiseEntries.removeValue(forKey: promiseID)
                self.outerframeHost.sendFilePromiseWriteResponse(requestID: requestID,
                                                                 promiseID: promiseID,
                                                                 localPath: targetURL.path,
                                                                 deleteWhenDone: true)
            } catch {
                try? FileManager.default.removeItem(at: targetDirectoryURL)
                guard let self else { return }
                self.dragStartedForSelectionIndex = nil
                self.statusLayer.string = "Could not prepare drag"
                self.filePromiseEntries.removeValue(forKey: promiseID)
                self.outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                                promiseID: promiseID,
                                                                errorMessage: String(describing: error))
                print("Files drag: failed to stage \(entry.path): \(error)")
            }
        }
    }

    private nonisolated static func writeDownload(from downloadURL: URL,
                                                  using urlSession: URLSession,
                                                  to targetURL: URL,
                                                  creating targetDirectoryURL: URL) async throws {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: targetDirectoryURL,
                                        withIntermediateDirectories: true)
        if fileManager.fileExists(atPath: targetURL.path) {
            try fileManager.removeItem(at: targetURL)
        }
        let fileDescriptor = open(targetURL.path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR)
        guard fileDescriptor >= 0 else {
            let errorCode = errno
            throw NSError(domain: NSCocoaErrorDomain,
                          code: NSFileWriteUnknownError,
                          userInfo: [
                            NSFilePathErrorKey: targetURL.path,
                            NSUnderlyingErrorKey: NSError(domain: NSPOSIXErrorDomain, code: Int(errorCode))
                          ])
        }

        let handle = FileHandle(fileDescriptor: fileDescriptor, closeOnDealloc: true)
        do {
            let (bytes, response) = try await urlSession.bytes(from: downloadURL)
            if let httpResponse = response as? HTTPURLResponse,
               !(200..<300).contains(httpResponse.statusCode) {
                throw NSError(domain: NSURLErrorDomain,
                              code: NSURLErrorBadServerResponse,
                              userInfo: [
                                NSLocalizedDescriptionKey: "Download failed with HTTP \(httpResponse.statusCode).",
                                NSURLErrorFailingURLErrorKey: downloadURL
                              ])
            }

            var buffer = Data()
            buffer.reserveCapacity(64 * 1024)
            for try await byte in bytes {
                buffer.append(byte)
                if buffer.count >= 64 * 1024 {
                    try handle.write(contentsOf: buffer)
                    buffer.removeAll(keepingCapacity: true)
                }
            }
            if !buffer.isEmpty {
                try handle.write(contentsOf: buffer)
            }
            try handle.close()
        } catch {
            try? handle.close()
            try? fileManager.removeItem(at: targetURL)
            throw error
        }
    }

    private nonisolated static func stageRemoteDirectory(path: String,
                                                         to targetURL: URL,
                                                         creating targetDirectoryURL: URL,
                                                         filesEndpoint: URL,
                                                         downloadEndpoint: URL,
                                                         using urlSession: URLSession,
                                                         depth: Int = 0) async throws {
        guard depth < 64 else {
            throw NSError(domain: NSCocoaErrorDomain,
                          code: NSFileReadUnknownError,
                          userInfo: [NSLocalizedDescriptionKey: "Directory nesting is too deep."])
        }

        let fileManager = FileManager.default
        try fileManager.createDirectory(at: targetDirectoryURL, withIntermediateDirectories: true)
        if fileManager.fileExists(atPath: targetURL.path) {
            try fileManager.removeItem(at: targetURL)
        }
        try fileManager.createDirectory(at: targetURL, withIntermediateDirectories: true)

        let response = try await remoteFileList(path: path,
                                                filesEndpoint: filesEndpoint,
                                                using: urlSession)
        for entry in response.entries {
            let childURL = targetURL.appendingPathComponent(Self.safeStagedFileName(entry.name),
                                                            isDirectory: entry.isDirectory)
            if entry.isDirectory {
                try await stageRemoteDirectory(path: entry.path,
                                               to: childURL,
                                               creating: targetURL,
                                               filesEndpoint: filesEndpoint,
                                               downloadEndpoint: downloadEndpoint,
                                               using: urlSession,
                                               depth: depth + 1)
            } else {
                let downloadURL = try remoteFileDownloadURL(path: entry.path,
                                                            downloadEndpoint: downloadEndpoint)
                try await writeDownload(from: downloadURL,
                                        using: urlSession,
                                        to: childURL,
                                        creating: targetURL)
            }
        }
    }

    private nonisolated static func remoteFileList(path: String,
                                                   filesEndpoint: URL,
                                                   using urlSession: URLSession) async throws -> FileListResponse {
        guard let request = binaryPathRequest(url: filesEndpoint,
                                              magic: FilePathRequestBinaryFormat.magic,
                                              path: path) else {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadURL,
                          userInfo: [NSLocalizedDescriptionKey: "Could not build directory listing URL."])
        }

        let (data, response) = try await urlSession.data(for: request)
        if let httpResponse = response as? HTTPURLResponse,
           !(200..<300).contains(httpResponse.statusCode) {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadServerResponse,
                          userInfo: [
                            NSLocalizedDescriptionKey: "Directory listing failed with HTTP \(httpResponse.statusCode).",
                            NSURLErrorFailingURLErrorKey: request.url as Any
                          ])
        }
        return try decodeFileList(data)
    }

    private nonisolated static func remoteFileDownloadURL(path: String,
                                                          downloadEndpoint: URL) throws -> URL {
        var components = URLComponents(url: downloadEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "path", value: path)]
        guard let url = components?.url else {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadURL,
                          userInfo: [NSLocalizedDescriptionKey: "Could not build download URL."])
        }
        return url
    }

    private nonisolated static func safeStagedFileName(_ name: String) -> String {
        let sanitized = name
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
        return sanitized.isEmpty ? "Untitled" : sanitized
    }

    private func fileTypeIdentifier(for entry: FileEntry) -> String {
        if entry.isDirectory {
            return UTType.folder.identifier
        }
        return fileTypeIdentifier(forFileName: entry.name)
    }

    private func fileTypeIdentifier(forFileName fileName: String) -> String {
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        if !fileExtension.isEmpty,
           let contentType = UTType(filenameExtension: fileExtension),
           !contentType.identifier.hasPrefix("dyn.") {
            return contentType.identifier
        }
        return UTType.data.identifier
    }

    private func fileIcon(for fileName: String) -> NSImage {
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        if !fileExtension.isEmpty,
           let contentType = UTType(filenameExtension: fileExtension) {
            return NSWorkspace.shared.icon(for: contentType)
        }
        return NSWorkspace.shared.icon(for: UTType.data)
    }

    private func rowIconCGImage(for entry: FileEntry, size: CGSize) -> CGImage? {
        if entry.isDirectory {
            return folderIconCGImage(size: size)
        }
        if entry.isExecutableFile {
            return terminalIconCGImage(size: size)
        }
        return fileIconCGImage(for: entry.name, size: size)
    }

    private func terminalIconCGImage(size: CGSize) -> CGImage? {
        let cacheKey = "\(Int(size.width.rounded()))x\(Int(size.height.rounded()))-\(Int(iconContentsScale))-\(appearance.name.rawValue)"
        if let cached = terminalIconCache[cacheKey] {
            return cached
        }

        let symbol = NSImage(systemSymbolName: "apple.terminal", accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: size.height, weight: .regular))
        symbol?.isTemplate = true
        let image = symbol
            ?? NSWorkspace.shared.icon(for: UTType.unixExecutable)
        let cgImage = renderedIconCGImage(image: image, size: size)
        if let cgImage {
            terminalIconCache[cacheKey] = cgImage
        }
        return cgImage
    }

    private func fileIconCGImage(for fileName: String, size: CGSize) -> CGImage? {
        let image = fileIcon(for: fileName).copy() as? NSImage ?? fileIcon(for: fileName)
        return renderedIconCGImage(image: image, size: size)
    }

    private func dragPreviewOrigin(forRowAt index: Int) -> CGPoint {
        let contentHeight = CGFloat(entries.count) * rowHeight
        let rowY = contentHeight - CGFloat(index + 1) * rowHeight
        let rowInRowsClip = CGRect(x: 0,
                                   y: rowsContentLayer.frame.minY + rowY,
                                   width: rowsClipLayer.bounds.width,
                                   height: rowHeight)
        let rowInRoot = rowsClipLayer.convert(rowInRowsClip, to: rootLayer)
        return CGPoint(x: rowInRoot.minX + horizontalInset,
                       y: rowInRoot.minY)
    }

    private func dragPreview(for entry: FileEntry) -> DragPreview? {
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        let font = NSFont.systemFont(ofSize: 13, weight: .regular)
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingMiddle
        let contentWidth = max(rowsClipLayer.bounds.width - horizontalInset * 2, 1)
        let nameWidth = max(floor(contentWidth * nameColumnWidth) - 24, 1)
        let measuredNameWidth = ceil((entry.name as NSString).size(withAttributes: [.font: font]).width)
        let textWidth = min(max(measuredNameWidth, 1), max(nameWidth, 1))
        let selectionPaddingX: CGFloat = 5
        let iconSize = CGSize(width: 16, height: 16)
        let iconX: CGFloat = 0
        let iconY: CGFloat = 5
        let labelX: CGFloat = 24
        let labelY: CGFloat = 5
        let selectionFrame = NSRect(x: labelX - selectionPaddingX,
                                    y: 3,
                                    width: textWidth + selectionPaddingX * 2,
                                    height: 20)
        let width = ceil(selectionFrame.maxX + 2)
        let height = rowHeight
        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: max(Int(ceil(width * scale)), 1),
                                            pixelsHigh: max(Int(ceil(height * scale)), 1),
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
            return nil
        }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.cgContext.scaleBy(x: scale, y: scale)
        defer {
            NSGraphicsContext.restoreGraphicsState()
        }

        appearance.performAsCurrentDrawingAppearance {
            NSColor.clear.setFill()
            NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: height)).fill()

            if let cgImage = rowIconCGImage(for: entry, size: iconSize) {
                let icon = NSImage(cgImage: cgImage, size: iconSize)
                icon.draw(in: NSRect(x: iconX, y: iconY, width: iconSize.width, height: iconSize.height))
            }

            NSColor.controlAccentColor.setFill()
            NSBezierPath(roundedRect: selectionFrame, xRadius: 4, yRadius: 4).fill()

            let titleAttributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: NSColor.white,
                .paragraphStyle: paragraph
            ]
            (entry.name as NSString).draw(in: NSRect(x: labelX,
                                                     y: labelY,
                                                     width: textWidth,
                                                     height: 17),
                                          withAttributes: titleAttributes)
        }

        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            return nil
        }
        return DragPreview(pngData: data, size: CGSize(width: width, height: height))
    }

    private func rowIndex(at point: CGPoint) -> Int {
        let localPoint = rowsClipLayer.convert(point, from: rootLayer)
        guard rowsClipLayer.bounds.contains(localPoint) else { return -1 }
        return Int(floor((rowsClipLayer.bounds.height - localPoint.y + scrollOffset) / rowHeight))
    }

    private func favoritePath(at point: CGPoint) -> String? {
        let localPoint = favoritesBarLayer.convert(point, from: rootLayer)
        guard favoritesBarLayer.bounds.contains(localPoint) else { return nil }

        for favorite in favoriteFrames where favorite.frame.contains(localPoint) {
            return favorite.path
        }
        return nil
    }

    private func favoritesBarContains(_ point: CGPoint) -> Bool {
        let localPoint = favoritesBarLayer.convert(point, from: rootLayer)
        return favoritesBarLayer.bounds.contains(localPoint)
    }

    private func breadcrumbPath(at point: CGPoint) -> String? {
        let localPoint = breadcrumbBarLayer.convert(point, from: rootLayer)
        guard breadcrumbBarLayer.bounds.contains(localPoint) else { return nil }

        for segment in breadcrumbSegmentFrames where segment.frame.contains(localPoint) {
            return segment.path
        }
        return nil
    }

    private func uploadDirectory(forDropAt point: CGPoint) -> String {
        let index = rowIndex(at: point)
        if index >= 0,
           index < entries.count,
           entries[index].isDirectory {
            return entries[index].path
        }
        return currentPath
    }

    private func handleDroppedPasteboardItems(_ items: [OuterframeContentPasteboardItem], at point: CGPoint) {
        let files = items.compactMap(decodeDroppedLocalFile)
        guard !files.isEmpty else { return }
        upload(files: files, to: uploadDirectory(forDropAt: point))
    }

    private func decodeDroppedLocalFile(_ item: OuterframeContentPasteboardItem) -> DroppedLocalFile? {
        guard let representation = item.representations.first(where: {
            $0.typeIdentifier == Self.droppedFileAccessPasteboardTypeIdentifier
        }) else {
            return nil
        }

        guard let payload = OuterframePasteboardPayload.decodeDroppedFileAccess(representation.data) else {
            return nil
        }

        return DroppedLocalFile(id: payload.id,
                                fileURL: URL(fileURLWithPath: payload.localPath, isDirectory: payload.isDirectory),
                                name: payload.name,
                                fileSize: payload.fileSize,
                                fileType: payload.fileType,
                                isDirectory: payload.isDirectory)
    }

    private func upload(files: [DroppedLocalFile], to directory: String) {
        guard let uploadEndpoint, let mkdirEndpoint, let urlSession else { return }
        statusLayer.string = "Uploading \(files.count) file\(files.count == 1 ? "" : "s")..."

        Task { [weak self, files, directory, uploadEndpoint, mkdirEndpoint, urlSession] in
            var hadFailure = false
            for file in files {
                do {
                    try await Self.uploadLocalItem(fileURL: file.fileURL,
                                                   name: file.name,
                                                   isDirectory: file.isDirectory,
                                                   to: directory,
                                                   uploadEndpoint: uploadEndpoint,
                                                   mkdirEndpoint: mkdirEndpoint,
                                                   using: urlSession)
                } catch {
                    hadFailure = true
                    print("Files upload: failed to upload \(file.fileURL.path): \(error)")
                }
                self?.outerframeHost.releaseDroppedFileAccess(file.id)
            }

            guard let self else { return }
            self.statusLayer.string = hadFailure ? "Upload failed" : ""
            self.fetchFiles(path: self.currentPath)
        }
    }

    private nonisolated static func uploadLocalItem(fileURL: URL,
                                                    name: String,
                                                    isDirectory: Bool,
                                                    to remoteDirectory: String,
                                                    uploadEndpoint: URL,
                                                    mkdirEndpoint: URL,
                                                    using urlSession: URLSession,
                                                    depth: Int = 0) async throws {
        guard depth < 64 else {
            throw NSError(domain: NSCocoaErrorDomain,
                          code: NSFileReadUnknownError,
                          userInfo: [NSLocalizedDescriptionKey: "Directory nesting is too deep."])
        }

        if isDirectory {
            try await createRemoteDirectory(parent: remoteDirectory,
                                            name: name,
                                            mkdirEndpoint: mkdirEndpoint,
                                            using: urlSession)
            let childRemoteDirectory = remoteChildPath(parent: remoteDirectory, name: name)
            let childURLs = try FileManager.default.contentsOfDirectory(at: fileURL,
                                                                        includingPropertiesForKeys: [.isDirectoryKey, .isRegularFileKey],
                                                                        options: [])
            for childURL in childURLs {
                let values = try childURL.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey])
                guard values.isDirectory == true || values.isRegularFile == true else { continue }
                try await uploadLocalItem(fileURL: childURL,
                                          name: childURL.lastPathComponent,
                                          isDirectory: values.isDirectory == true,
                                          to: childRemoteDirectory,
                                          uploadEndpoint: uploadEndpoint,
                                          mkdirEndpoint: mkdirEndpoint,
                                          using: urlSession,
                                          depth: depth + 1)
            }
            return
        }

        var components = URLComponents(url: uploadEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "directory", value: remoteDirectory),
            URLQueryItem(name: "name", value: name)
        ]
        guard let url = components?.url else {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadURL,
                          userInfo: [NSLocalizedDescriptionKey: "Could not build upload URL."])
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        let (_, response) = try await urlSession.upload(for: request, fromFile: fileURL)
        if let http = response as? HTTPURLResponse,
           !(200..<300).contains(http.statusCode) {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadServerResponse,
                          userInfo: [NSLocalizedDescriptionKey: "Upload failed with HTTP \(http.statusCode)."])
        }
    }

    private nonisolated static func createRemoteDirectory(parent: String,
                                                          name: String,
                                                          mkdirEndpoint: URL,
                                                          using urlSession: URLSession) async throws {
        guard let request = binaryDirectoryNameRequest(url: mkdirEndpoint,
                                                       magic: FileMkdirRequestBinaryFormat.magic,
                                                       directory: parent,
                                                       name: name) else {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadURL,
                          userInfo: [NSLocalizedDescriptionKey: "Could not build mkdir URL."])
        }

        let (_, response) = try await urlSession.data(for: request)
        if let http = response as? HTTPURLResponse,
           !(200..<300).contains(http.statusCode) {
            throw NSError(domain: NSURLErrorDomain,
                          code: NSURLErrorBadServerResponse,
                          userInfo: [NSLocalizedDescriptionKey: "Create directory failed with HTTP \(http.statusCode)."])
        }
    }

    private nonisolated static func remoteChildPath(parent: String, name: String) -> String {
        if parent == "/" {
            return "/" + name
        }
        return parent + "/" + name
    }

    private func makeTextLayer(size: CGFloat,
                               weight: NSFont.Weight,
                               alignment: CATextLayerAlignmentMode = .left) -> CATextLayer {
        let layer = CATextLayer()
        layer.font = NSFont.systemFont(ofSize: size, weight: weight)
        layer.fontSize = size
        layer.contentsScale = 2
        layer.truncationMode = .end
        layer.alignmentMode = alignment
        layer.foregroundColor = NSColor.labelColor.cgColor
        return layer
    }

    private func folderIconCGImage(size: CGSize) -> CGImage? {
        let image = NSWorkspace.shared.icon(for: UTType.folder).copy() as? NSImage
            ?? NSWorkspace.shared.icon(for: UTType.folder)
        return renderedIconCGImage(image: image, size: size)
    }

    private func renderedIconCGImage(image: NSImage, size: CGSize) -> CGImage? {
        let scale = iconContentsScale
        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: max(Int(ceil(size.width * scale)), 1),
                                            pixelsHigh: max(Int(ceil(size.height * scale)), 1),
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
            return nil
        }

        bitmap.size = size
        image.size = size

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.imageInterpolation = .high
        context.cgContext.scaleBy(x: scale, y: scale)
        defer {
            NSGraphicsContext.restoreGraphicsState()
        }

        appearance.performAsCurrentDrawingAppearance {
            NSColor.clear.setFill()
            NSBezierPath(rect: NSRect(origin: .zero, size: size)).fill()
            NSColor.labelColor.set()
            image.draw(in: NSRect(origin: .zero, size: size),
                       from: .zero,
                       operation: .sourceOver,
                       fraction: 1,
                       respectFlipped: false,
                       hints: [.interpolation: NSImageInterpolation.high])
        }

        return bitmap.cgImage
    }

    private func textWidth(_ text: String, fontSize: CGFloat, weight: NSFont.Weight) -> CGFloat {
        let font = NSFont.systemFont(ofSize: fontSize, weight: weight)
        return ceil((text as NSString).size(withAttributes: [.font: font]).width)
    }

    private func alternatingRowColors() -> (even: CGColor, odd: CGColor) {
        let alternating = NSColor.alternatingContentBackgroundColors
        let evenColor = NSColor.clear.cgColor
        let oddColor: CGColor
        if alternating.count >= 2 {
            oddColor = alternating[1].cgColor
        } else if let first = alternating.first {
            oddColor = first.cgColor
        } else {
            let background = NSColor.controlBackgroundColor
            oddColor = (background.blended(withFraction: 0.08, of: NSColor.labelColor) ?? background).cgColor
        }
        return (even: evenColor, odd: oddColor)
    }

    private func formatModified(_ timestamp: Double) -> String {
        let date = Date(timeIntervalSince1970: timestamp)
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    private func formatByteCount(_ value: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }

    private func buildAccessibilitySnapshot() -> OuterframeAccessibilitySnapshot {
        var nextIdentifier: UInt32 = 1
        var children: [OuterframeAccessibilityNode] = []

        children.append(contentsOf: buildFavoritesAccessibilityNodes(nextIdentifier: &nextIdentifier))
        children.append(contentsOf: buildBreadcrumbAccessibilityNodes(nextIdentifier: &nextIdentifier))
        children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                          role: .staticText,
                                          frame: headerLayer.convert(nameHeaderLayer.frame, to: rootLayer),
                                          label: "Name"))
        children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                          role: .staticText,
                                          frame: headerLayer.convert(modifiedHeaderLayer.frame, to: rootLayer),
                                          label: "Modified"))
        children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                          role: .staticText,
                                          frame: headerLayer.convert(sizeHeaderLayer.frame, to: rootLayer),
                                          label: "Size"))
        children.append(buildFileTableAccessibilityNode(nextIdentifier: &nextIdentifier))

        if let status = statusLayer.string as? String, !status.isEmpty {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                             role: .staticText,
                                             frame: statusLayer.frame,
                                             label: status))
        }

        let rootNode = OuterframeAccessibilityNode(identifier: 0,
                                                   role: .container,
                                                   frame: rootLayer.bounds,
                                                   label: "Files",
                                                   children: children)
        return OuterframeAccessibilitySnapshot(rootNodes: [rootNode])
    }

    private func buildFavoritesAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        let favorites = [(title: "Home", path: homePath ?? "~")] + favoriteLocations.map { (title: $0.title, path: $0.path) }
        var nodes: [OuterframeAccessibilityNode] = []
        for (index, favorite) in favorites.enumerated() where index < favoriteFrames.count {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: favoritesBarLayer.convert(favoriteFrames[index].frame, to: rootLayer),
                                           label: favorite.title,
                                           value: favorite.path,
                                           hint: favorite.path == currentPath ? "Current location" : "Open location"))
        }
        return nodes
    }

    private func buildBreadcrumbAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        let segments = breadcrumbSegments()
        var nodes: [OuterframeAccessibilityNode] = []
        for (index, segment) in segments.enumerated() where index < breadcrumbSegmentFrames.count {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: breadcrumbBarLayer.convert(breadcrumbSegmentFrames[index].frame, to: rootLayer),
                                           label: segment.title,
                                           value: segment.path,
                                           hint: segment.path == currentPath ? "Current folder" : "Open folder"))
        }
        return nodes
    }

    private func buildFileTableAccessibilityNode(nextIdentifier: inout UInt32) -> OuterframeAccessibilityNode {
        var rowNodes: [OuterframeAccessibilityNode] = []
        let contentWidth = max(rowsClipLayer.bounds.width - horizontalInset * 2, 1)
        let nameWidth = floor(contentWidth * nameColumnWidth)
        let modifiedWidth = floor(contentWidth * modifiedColumnWidth)
        let sizeWidth = max(contentWidth - nameWidth - modifiedWidth, 1)
        let visibleStart = max(Int(floor(scrollOffset / rowHeight)), 0)
        let visibleCount = Int(ceil(rowsClipLayer.bounds.height / rowHeight)) + 2
        let visibleEnd = min(entries.count, visibleStart + visibleCount)

        if visibleStart < visibleEnd {
            for index in visibleStart..<visibleEnd {
                let entry = entries[index]
                let top = rowsClipLayer.bounds.height - CGFloat(index) * rowHeight + scrollOffset - rowHeight
                let rowFrame = CGRect(x: 0, y: top, width: rowsClipLayer.bounds.width, height: rowHeight)
                let type = entry.isDirectory ? "Folder" : "File"
                let size = entry.isDirectory ? "" : formatByteCount(entry.size)
                let selectedPrefix = selectedIndex == index ? "Selected, " : ""
                let rowLabel = "\(selectedPrefix)\(entry.name), \(type), modified \(formatModified(entry.modified))\(size.isEmpty ? "" : ", \(size)")"
                let cells = [
                    accessibilityNode(nextIdentifier: &nextIdentifier,
                                      role: .cell,
                                      frame: rowsClipLayer.convert(CGRect(x: horizontalInset, y: top, width: nameWidth, height: rowHeight), to: rootLayer),
                                      label: "Name",
                                      value: entry.name),
                    accessibilityNode(nextIdentifier: &nextIdentifier,
                                      role: .cell,
                                      frame: rowsClipLayer.convert(CGRect(x: horizontalInset + nameWidth, y: top, width: modifiedWidth, height: rowHeight), to: rootLayer),
                                      label: "Modified",
                                      value: formatModified(entry.modified)),
                    accessibilityNode(nextIdentifier: &nextIdentifier,
                                      role: .cell,
                                      frame: rowsClipLayer.convert(CGRect(x: horizontalInset + nameWidth + modifiedWidth, y: top, width: sizeWidth, height: rowHeight), to: rootLayer),
                                      label: "Size",
                                      value: entry.isDirectory ? "Folder" : size)
                ]
                rowNodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                                 role: .row,
                                                 frame: rowsClipLayer.convert(rowFrame, to: rootLayer),
                                                 label: rowLabel,
                                                 value: entry.path,
                                                 hint: entry.isDirectory ? "Open folder" : "Open file",
                                                 children: cells))
            }
        }

        return accessibilityNode(nextIdentifier: &nextIdentifier,
                                 role: .table,
                                 frame: rowsClipLayer.frame,
                                 label: "Files in \(currentPath)",
                                 children: rowNodes,
                                 rowCount: entries.count,
                                 columnCount: 3)
    }

    private func accessibilityNode(nextIdentifier: inout UInt32,
                                   role: OuterframeAccessibilityRole,
                                   frame: CGRect,
                                   label: String? = nil,
                                   value: String? = nil,
                                   hint: String? = nil,
                                   children: [OuterframeAccessibilityNode] = [],
                                   rowCount: Int? = nil,
                                   columnCount: Int? = nil,
                                   isEnabled: Bool = true) -> OuterframeAccessibilityNode {
        let identifier = nextIdentifier
        nextIdentifier = nextIdentifier == UInt32.max ? 1 : nextIdentifier + 1
        return OuterframeAccessibilityNode(identifier: identifier,
                                           role: role,
                                           frame: frame,
                                           label: label,
                                           value: value,
                                           hint: hint,
                                           children: children,
                                           rowCount: rowCount,
                                           columnCount: columnCount,
                                           isEnabled: isEnabled)
    }

    private func notifyAccessibilityLayoutChanged() {
        guard hasRegisteredLayer, !accessibilityNotificationScheduled else { return }
        accessibilityNotificationScheduled = true
        Task { @MainActor in
            accessibilityNotificationScheduled = false
            outerframeHost.notifyAccessibilityTreeChanged(.layoutChanged)
        }
    }
}

private func withoutImplicitAnimations(_ body: () -> Void) {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    body()
    CATransaction.commit()
}

@MainActor
private final class FilesRowsScrollbarDelegate: ScrollbarControllerDelegate {
    private weak var owner: FilesHandler?

    init(owner: FilesHandler) {
        self.owner = owner
    }

    func scrollbarDidChangeScrollOffset(_ offset: CGFloat) {
        owner?.setRowsScroll(offset)
    }
}
