// DiskPrism UI — runs a scan off the main thread and reports back on it.
#import <Foundation/Foundation.h>

#include "BulkScanner.hpp"
#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
}

NS_ASSUME_NONNULL_BEGIN

/// Live counters, delivered on the main thread at the scanner's throttled cadence (~18 Hz).
typedef void (^DPScanProgressHandler)(diskprism::ScanProgress progress);

/// Result of a scan, delivered on the main thread. `arena` is a freshly built tree that
/// the receiver now owns (wrap it in a `std::unique_ptr`); it is null only if the root
/// could not be scanned at all.
typedef void (^DPScanCompletionHandler)(diskprism::FlatArena *_Nullable arena, diskprism::ScanResult result);

/// Owns the background work of one window: at most one scan at a time.
///
/// The UI never sees a tree that is still being built — the arena is created and filled
/// on the scan thread and handed over, complete and finalized, in the completion handler.
@interface DPScanCoordinator : NSObject

@property (nonatomic, readonly, getter=isScanning) BOOL scanning;

/// Starts scanning `path`. A scan that is still running is abandoned: it is cancelled and
/// its handlers are never called.
- (void)scanPath:(NSString *)path
      sortMetric:(diskprism::SizeMetric)sortMetric
        progress:(DPScanProgressHandler)progress
      completion:(DPScanCompletionHandler)completion;

/// Asks the running scan to stop. Its completion handler is still called, with status
/// `Cancelled` and the partial tree gathered so far.
- (void)cancel;

@end

NS_ASSUME_NONNULL_END
