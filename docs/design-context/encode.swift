// Encode the native SDL RGB stream using macOS AVFoundation (no screen capture).
import Foundation
import AVFoundation
import CoreVideo

let fifo = CommandLine.arguments[1]
let output = URL(fileURLWithPath: CommandLine.arguments[2])
try? FileManager.default.removeItem(at: output)
let writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: 480, AVVideoHeightKey: 640,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 2400000,
                                    AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel]
])
input.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey as String: 480, kCVPixelBufferHeightKey as String: 640
])
writer.add(input)
guard writer.startWriting() else { fatalError("Cannot start encoder: \(String(describing: writer.error))") }
writer.startSession(atSourceTime: .zero)
let reader = try FileHandle(forReadingFrom: URL(fileURLWithPath: fifo))
let bytesPerFrame = 960 * 1280 * 3
var frame: Int64 = 0
while true {
    var data = Data()
    while data.count < bytesPerFrame {
        let part = try reader.read(upToCount: bytesPerFrame - data.count) ?? Data()
        if part.isEmpty { break }
        data.append(part)
    }
    if data.isEmpty { break }
    guard data.count == bytesPerFrame else { fatalError("Partial frame") }
    while !input.isReadyForMoreMediaData {
        guard writer.status == .writing else { fatalError("Encoder failed: \(String(describing: writer.error))") }
        Thread.sleep(forTimeInterval: 0.002)
    }
    var optionalBuffer: CVPixelBuffer?
    guard let pool = adaptor.pixelBufferPool,
          CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optionalBuffer) == kCVReturnSuccess,
          let buffer = optionalBuffer else { fatalError("No video pixel buffer") }
    CVPixelBufferLockBaseAddress(buffer, [])
    let destination = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(buffer)
    data.withUnsafeBytes { raw in
        let source = raw.bindMemory(to: UInt8.self)
        for y in 0..<640 {
            for x in 0..<480 {
                let s = (y * 2 * 960 + x * 2) * 3
                let d = y * stride + x * 4
                for c in 0..<3 {
                    let sum = Int(source[s+c]) + Int(source[s+3+c]) + Int(source[s+2880+c]) + Int(source[s+2883+c])
                    destination[d+2-c] = UInt8(sum / 4)
                }
                destination[d+3] = 255
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, [])
    guard adaptor.append(buffer, withPresentationTime: CMTime(value: frame, timescale: 20)) else {
        fatalError("Cannot append frame: \(String(describing: writer.error))")
    }
    frame += 1
}
writer.endSession(atSourceTime: CMTime(value: frame, timescale: 20))
input.markAsFinished()
let finished = DispatchSemaphore(value: 0)
writer.finishWriting { finished.signal() }
finished.wait()
guard writer.status == .completed else { fatalError("Cannot finish: \(String(describing: writer.error))") }
print("Encoded \(frame) frames")
