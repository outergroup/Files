from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'Frontend/FilesContent.swift').read_text()
parts='import Foundation\n'+source[source.index('private struct FileListResponse'):source.index('private struct DragPreview')]+source[source.index('private enum FilesEnvironment {'):]
test=r'''
let driver = CommandLine.arguments[1]
let folder = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent(UUID().uuidString)
try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: folder) }
let destination = folder.appendingPathComponent("Folder & + # % 雪")
try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
let file = destination.appendingPathComponent("hello.txt")
try Data("Container-aware Files test".utf8).write(to: file)
func call(_ operation: UInt32, path: String, target: String = "host") throws -> Data {
    let process = Process(), input = Pipe(), output = Pipe()
    process.executableURL = URL(fileURLWithPath: driver)
    process.environment = ProcessInfo.processInfo.environment.merging(["XDG_CONFIG_HOME": folder.appendingPathComponent("config").path]) { _, b in b }
    process.standardInput = input; process.standardOutput = output
    try process.run()
    guard let url = URL(string: "http://localhost/api/environments"), let request = FilesEnvironment.request(url: url, operation: operation, target: target, path: path), let bytes = request.httpBody else { fatalError("encode") }
    input.fileHandleForWriting.write(bytes); try input.fileHandleForWriting.close()
    let bytesOut = output.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit(); precondition(process.terminationStatus == 0)
    return bytesOut
}
private let listing = try FilesEnvironment.list(call(1, path: destination.path), operation: 1)
precondition(listing.path.hasSuffix(folder.lastPathComponent + "/" + destination.lastPathComponent) && listing.entries.contains { $0.name == "hello.txt" }, "Expected \(destination.path), got \(listing.path): \(listing.entries.map { $0.name })")
precondition(listing.entries.first { $0.name == "hello.txt" }?.modified ?? 0 > 1_000_000_000)
_ = try FilesEnvironment.envelope(call(11, path: destination.path))
_ = try FilesEnvironment.envelope(call(11, path: destination.path))
private let places = try FilesEnvironment.list(call(10, path: "~"), operation: 10)
precondition(places.entries.filter { $0.path == listing.path }.count == 1)
let preview = try FilesEnvironment.envelope(call(8, path: file.path))
precondition(String(data: preview.body, encoding: .utf8) == "Container-aware Files test")
let targets = try FilesEnvironment.targets(call(7, path: ""))
precondition(targets.targets.contains { $0.id == "host" })
do { _ = try FilesEnvironment.list(call(1, path: destination.path, target: "docker:missing-container"), operation: 1); fatalError("Must not fall back to host") } catch {}
let valid = try call(1, path: destination.path)
for size in [0, 12, 47, valid.count - 1] {
    do { _ = try FilesEnvironment.list(Data(valid.prefix(size)), operation: 1); fatalError("Truncated payload accepted") } catch {}
}
var corrupt = valid
corrupt[16] = 0xff; corrupt[17] = 0xff; corrupt[18] = 0xff; corrupt[19] = 0xff
do { _ = try FilesEnvironment.list(corrupt, operation: 1); fatalError("Bad offset accepted") } catch {}
print("PASS native environment codec: listing, persistent favorites, preview, discovery, no host fallback, malformed payloads")
'''
import subprocess, tempfile
with tempfile.TemporaryDirectory(prefix='files-native-environment-') as temporary:
    work = Path(temporary)
    (work/'main.swift').write_text(parts+test)
    subprocess.run(['cc', '-std=gnu17', '-O2', str(root/'Tests/EnvironmentDriver.c'), str(root/'Backend/Environment.c'), '-lpthread', '-lm', '-o', str(work/'driver')], check=True)
    subprocess.run(['swiftc', str(work/'main.swift'), '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test'), str(work/'driver')], check=True)
