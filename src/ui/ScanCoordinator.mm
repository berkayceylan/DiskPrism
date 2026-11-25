#import "ScanCoordinator.h"

#include "FlatArena.hpp"

#include <memory>
#include <new>
#include <string>

using namespace diskprism;

@implementation DPScanCoordinator {
    std::shared_ptr<BulkScanner> _scanner;  // main thread only; the scan thread holds its own reference
    uint64_t _generation;                   // main thread only
    BOOL _scanning;
}

- (BOOL)isScanning {
    return _scanning;
}

- (void)scanPath:(NSString *)path
      sortMetric:(SizeMetric)sortMetric
        progress:(DPScanProgressHandler)progress
      completion:(DPScanCompletionHandler)completion {
    NSAssert(NSThread.isMainThread, @"DPScanCoordinator is driven from the main thread");

    // Abandon whatever is running: bumping the generation orphans its callbacks.
    if (_scanner != nullptr) _scanner->cancel();
    const uint64_t generation = ++_generation;
    const std::shared_ptr<BulkScanner> scanner = std::make_shared<BulkScanner>();
    _scanner = scanner;
    _scanning = YES;

    const std::string root = path.fileSystemRepresentation;
    DPScanProgressHandler progressHandler = [progress copy];
    DPScanCompletionHandler completionHandler = [completion copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ScanResult result;
        FlatArena *arena = nullptr;
        try {
            arena = new FlatArena();
            ScanOptions options;
            options.sort_metric = sortMetric;
            result = scanner->scan(root, *arena, options, [self, generation, progressHandler](const ScanProgress &snapshot) {
                if (snapshot.finished) return;  // the completion handler reports the totals
                const ScanProgress copy = snapshot;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self->_generation == generation) progressHandler(copy);
                });
            });
        } catch (const std::bad_alloc &) {
            delete arena;
            arena = nullptr;
            result.status = ScanStatus::InvalidRoot;
            result.error_message = "not enough memory (address space) to hold the file tree";
        }
        if (arena != nullptr && result.status == ScanStatus::InvalidRoot) {
            delete arena;
            arena = nullptr;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_generation != generation) {
                delete arena;  // superseded by a newer scan
                return;
            }
            self->_scanning = NO;
            self->_scanner.reset();
            completionHandler(arena, result);
        });
    });
}

- (void)cancel {
    if (_scanner != nullptr) _scanner->cancel();
}

@end
