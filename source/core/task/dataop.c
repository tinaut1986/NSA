#include <malloc.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "dataop.h"
#include "../core.h"
#include "../speedlog.h"
#include "../xferturn.h"

void data_op_progress_items(const data_op_data* op, float* progress, char* text) {
    *progress = op->total > 0 ? (float) op->processed / (float) op->total : 0;
    snprintf(text, PROGRESS_TEXT_MAX, _("%lu / %lu"), op->processed, op->total);
}

void data_op_progress_bytes(const data_op_data* op, float* progress, char* text, bool items) {
    *progress = op->currTotal != 0 ? (float) ((double) op->currProcessed / (double) op->currTotal) : 0;
    if(items) {
        snprintf(text, PROGRESS_TEXT_MAX, _("%lu / %lu\n%.2f %s / %.2f %s\n%.2f %s/s"), op->processed, op->total,
                 ui_get_display_size(op->currProcessed), ui_get_display_size_units(op->currProcessed),
                 ui_get_display_size(op->currTotal), ui_get_display_size_units(op->currTotal),
                 ui_get_display_size(op->bytesPerSecond), ui_get_display_size_units(op->bytesPerSecond));
    } else {
        snprintf(text, PROGRESS_TEXT_MAX, _("%.2f %s / %.2f %s\n%.2f %s/s"),
                 ui_get_display_size(op->currProcessed), ui_get_display_size_units(op->currProcessed),
                 ui_get_display_size(op->currTotal), ui_get_display_size_units(op->currTotal),
                 ui_get_display_size(op->bytesPerSecond), ui_get_display_size_units(op->bytesPerSecond));
    }
}

static Result task_data_op_check_running(data_op_data* data) {
    Result res = 0;

    if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
        res = R_APP_CANCELLED;
    } else {
        bool suspended = svcWaitSynchronization(task_get_suspend_event(), 0) != 0;
        if(suspended) {
            if(data->suspend != NULL && R_SUCCEEDED(res)) {
                res = data->suspend(data->data, data->processed);
            }
        }

        svcWaitSynchronization(task_get_pause_event(), U64_MAX);

        if(suspended) {
            if(data->restore != NULL && R_SUCCEEDED(res)) {
                res = data->restore(data->data, data->processed);
            }
        }
    }

    return res;
}

static Result task_data_op_copy(data_op_data* data, u32 index) {
    data->currProcessed = 0;
    data->currTotal = 0;

    data->bytesPerSecond = 0;
    data->estimatedRemainingSeconds = 0;

    Result res = 0;

    bool isDir = false;
    if(R_SUCCEEDED(res = data->isSrcDirectory(data->data, index, &isDir)) && isDir) {
        res = data->makeDstDirectory(data->data, index);
    } else {
        u32 srcHandle = 0;
        if(R_SUCCEEDED(res = data->openSrc(data->data, index, &srcHandle))) {
            if(R_SUCCEEDED(res = data->getSrcSize(data->data, srcHandle, &data->currTotal))) {
                if(data->currTotal == 0) {
                    if(data->copyEmpty) {
                        u32 dstHandle = 0;
                        if(R_SUCCEEDED(res = data->openDst(data->data, index, NULL, data->currTotal, &dstHandle))) {
                            res = data->closeDst(data->data, index, true, dstHandle);
                        }
                    } else {
                        res = R_APP_BAD_DATA;
                    }
                } else {
                    u8* buffer = (u8*) calloc(1, data->bufferSize);
                    if(buffer != NULL) {
                        u32 dstHandle = 0;

                        // speed-log.txt: reading the source counts as "net", opening, writing and closing the
                        // destination (an install's AM calls) as "disk".
                        char traceName[32];
                        snprintf(traceName, sizeof(traceName), "item %lu", index);
                        speedlog* trace = speedlog_begin("copy", traceName, data->currTotal);

                        u64 ioStartTime = 0;
                        u64 lastBytesPerSecondUpdate = osGetTime();
                        u32 bytesSinceUpdate = 0;

                        bool firstRun = true;
                        while(data->currProcessed < data->currTotal) {
                            if(R_FAILED(res = task_data_op_check_running(data))) {
                                break;
                            }

                            u32 bytesRead = 0;
                            u64 ticks = speedlog_ticks();
                            if(R_FAILED(res = data->readSrc(data->data, srcHandle, &bytesRead, buffer, data->currProcessed, data->bufferSize))) {
                                break;
                            }

                            speedlog_net(trace, bytesRead, speedlog_ticks() - ticks);

                            if(firstRun) {
                                firstRun = false;

                                ticks = speedlog_ticks();
                                if(R_FAILED(res = data->openDst(data->data, index, buffer, data->currTotal, &dstHandle))) {
                                    break;
                                }

                                speedlog_disk(trace, 0, speedlog_ticks() - ticks);
                            }

                            u32 bytesWritten = 0;
                            ticks = speedlog_ticks();
                            if(R_FAILED(res = data->writeDst(data->data, dstHandle, &bytesWritten, buffer, data->currProcessed, bytesRead))) {
                                break;
                            }

                            speedlog_disk(trace, bytesWritten, speedlog_ticks() - ticks);

                            data->currProcessed += bytesWritten;
                            bytesSinceUpdate += bytesWritten;

                            u64 time = osGetTime();
                            u64 elapsed = time - lastBytesPerSecondUpdate;
                            if(elapsed >= 1000) {
                                data->bytesPerSecond = (u32) (bytesSinceUpdate / (elapsed / 1000.0f));

                                if(ioStartTime != 0) {
                                    data->estimatedRemainingSeconds = (u32) ((data->currTotal - data->currProcessed) / (data->currProcessed / ((time - ioStartTime) / 1000.0f)));
                                } else {
                                    data->estimatedRemainingSeconds = 0;
                                }

                                if(ioStartTime == 0 && data->currProcessed > 0) {
                                    ioStartTime = time;
                                }

                                bytesSinceUpdate = 0;
                                lastBytesPerSecondUpdate = time;
                            }
                        }

                        if(dstHandle != 0) {
                            u64 ticks = speedlog_ticks();
                            Result closeDstRes = data->closeDst(data->data, index, res == 0, dstHandle);
                            speedlog_disk(trace, 0, speedlog_ticks() - ticks);
                            if(R_SUCCEEDED(res)) {
                                res = closeDstRes;
                            }
                        }

                        speedlog_end(trace, R_SUCCEEDED(res));
                        free(buffer);
                    } else {
                        res = R_APP_OUT_OF_MEMORY;
                    }
                }
            }

            Result closeSrcRes = data->closeSrc(data->data, index, res == 0, srcHandle);
            if(R_SUCCEEDED(res)) {
                res = closeSrcRes;
            }
        }
    }

    return res;
}

typedef struct {
    data_op_data* data;

    u32 index;

    u32 dstHandle;
    bool firstRun;
    u64 ioStartTime;
    u64 lastBytesPerSecondUpdate;
    u32 bytesSinceUpdate;

    u64 writeOffset;

    u32 turn;   // The transfer turn (xferturn.h) the download holds.
    u64 from;   // Where it starts in the file: set by the download before the first bytes.
} data_op_download_data;

static Result task_data_op_download_callback(void* userData, void* buffer, size_t size) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    if(downloadData->firstRun) {
        downloadData->firstRun = false;
        downloadData->writeOffset = downloadData->from;
        data->currResumeFrom = downloadData->from;

        Result res = data->openDst(data->data, downloadData->index, buffer, data->currTotal, &downloadData->dstHandle);
        if(R_FAILED(res)) {
            return res;
        }
    }

    u32 bytesWritten = 0;
    Result res = data->writeDst(data->data, downloadData->dstHandle, &bytesWritten, buffer, downloadData->writeOffset, size);
    downloadData->writeOffset += bytesWritten;
    data->currWritten = downloadData->writeOffset;

    return res;
}

static Result task_data_op_download_open(void* userData, void* first, size_t size, u64 total) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    downloadData->firstRun = false;
    data->currTotal = total;
    return data->openDst(data->data, downloadData->index, first, total, &downloadData->dstHandle);
}

static Result task_data_op_download_write_at(void* userData, u64 offset, void* buffer, size_t size) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    u32 bytesWritten = 0;
    Result res = data->writeDst(data->data, downloadData->dstHandle, &bytesWritten, buffer, offset, size);
    return R_SUCCEEDED(res) && bytesWritten != size ? R_APP_IO_ERROR : res;
}

static Result task_data_op_download_check_running(void* userData) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;

    return task_data_op_check_running(downloadData->data);
}

static Result task_data_op_download_progress(void* userData, u64 total, u64 curr, u32 connections) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    data->connections = connections;

    // A download that resumes counts from where it started.
    if(data->currProcessed < downloadData->from) {
        data->currProcessed = downloadData->from;
    }

    downloadData->bytesSinceUpdate += curr > data->currProcessed ? curr - data->currProcessed : 0;

    data->currTotal = total;
    data->currProcessed = curr;
    xferturn_total(downloadData->turn, total);
    xferturn_progress(downloadData->turn, curr);

    u64 time = osGetTime();
    u64 elapsed = time - downloadData->lastBytesPerSecondUpdate;
    if(elapsed >= 1000) {
        // Downloads come in bursts (several connections, segments): an average
        // over the last seconds reads steadier than the last second alone.
        u32 now = (u32) (downloadData->bytesSinceUpdate / (elapsed / 1000.0f));
        data->bytesPerSecond = data->bytesPerSecond == 0 ? now : (u32) (data->bytesPerSecond * 0.75f + now * 0.25f);

        if(downloadData->ioStartTime != 0) {
            u64 done = data->currProcessed > downloadData->from ? data->currProcessed - downloadData->from : 0;
            data->estimatedRemainingSeconds = done > 0 ? (u32) ((data->currTotal - data->currProcessed) / (done / ((time - downloadData->ioStartTime) / 1000.0f))) : 0;
        } else {
            data->estimatedRemainingSeconds = 0;
        }

        if(downloadData->ioStartTime == 0 && data->currProcessed > downloadData->from) {
            downloadData->ioStartTime = time;
        }

        downloadData->bytesSinceUpdate = 0;
        downloadData->lastBytesPerSecondUpdate = time;
    }

    return 0;
}

static Result task_data_op_download(data_op_data* data, u32 index) {
    data->currProcessed = 0;
    data->currTotal = 0;

    data->bytesPerSecond = 0;
    data->estimatedRemainingSeconds = 0;
    data->connections = 0;
    data->currResumeFrom = 0;
    data->currWritten = 0;

    Result res = 0;

    char url[DOWNLOAD_URL_MAX];
    if(R_SUCCEEDED(res = data->getSrcUrl(data->data, index, url, DOWNLOAD_URL_MAX))) {
        data_op_download_data downloadData = {data, index, 0, true, 0, osGetTime(), 0, 0, 0, 0};
        // One transfer at a time: an FTP, web page or console to console transfer under way is let finish.
        if((downloadData.turn = xferturn_take(XFER_DOWNLOAD, url, 0)) == 0) {
            return R_APP_BUSY;
        }

        // What an earlier try left (saving, in order): from there when the server allows.
        u64 from = 0;
        u64 expected = 0;
        if(!data->dstAnyOrder && data->resumeFrom != NULL) {
            data->resumeFrom(data->data, index, &from, &expected);
        }

        if(data->dstAnyOrder) {
            res = http_download_file(url, data->bufferSize, &downloadData, task_data_op_download_open, task_data_op_download_write_at,
                                     task_data_op_download_check_running, task_data_op_download_progress);
        } else if(from > 0) {
            downloadData.from = from;
            res = http_download_callback_from(url, data->bufferSize, &downloadData.from, expected, &downloadData, task_data_op_download_callback,
                                              task_data_op_download_check_running, task_data_op_download_progress);
        } else {
            res = http_download_callback(url, data->bufferSize, &downloadData, task_data_op_download_callback, task_data_op_download_check_running, task_data_op_download_progress);
        }
        xferturn_release(downloadData.turn);

        if(downloadData.dstHandle != 0) {
            Result closeDstRes = data->closeDst(data->data, index, res == 0, downloadData.dstHandle);
            if(R_SUCCEEDED(res)) {
                res = closeDstRes;
            }
        }
    }

    return res;
}

static Result task_data_op_delete(data_op_data* data, u32 index) {
    return data->delete(data->data, index);
}

static void task_data_op_retry_onresponse(ui_view* view, void* data, u32 response) {
    ((data_op_data*) data)->retryResponse = response == PROMPT_YES;
}

static void task_data_op_thread(void* arg) {
    data_op_data* data = (data_op_data*) arg;

    data->processed = 0;
    while(data->processed < data->total) {
        Result res = 0;
        u32 next = data->processed + 1;

        if(R_SUCCEEDED(res = task_data_op_check_running(data))) {
            switch(data->op) {
                case DATAOP_COPY:
                    res = task_data_op_copy(data, data->processed);
                    break;
                case DATAOP_DOWNLOAD:
                    res = task_data_op_download(data, data->processed);
                    break;
                case DATAOP_DELETE:
                    res = task_data_op_delete(data, data->processed);
                    break;
                default:
                    break;
            }
        }

        data->result = res;

        if(R_FAILED(res)) {
            if(res == R_APP_CANCELLED) {
                if(!data->quietCancel) {
                    prompt_display_notify("Cancelled", "Operation cancelled.", COLOR_TEXT, NULL, NULL, NULL);
                }

                break;
            } else if(res != R_APP_SKIPPED) {
                ui_view* errorView = NULL;
                bool proceed = data->error(data->data, data->processed, res, &errorView);

                if(errorView != NULL) {
                    svcWaitSynchronization(errorView->active, U64_MAX);
                }

                if(data->noRetry) {
                    break;
                }

                ui_view* retryView = prompt_display_yes_no("Confirmation", "Retry?", COLOR_TEXT, data, NULL, task_data_op_retry_onresponse);
                if(retryView != NULL) {
                    svcWaitSynchronization(retryView->active, U64_MAX);

                    if(data->retryResponse) {
                        // Again from this item, or from the first one.
                        next = proceed ? data->processed : 0;
                    } else if(!proceed) {
                        break;
                    }
                } else if(!proceed) {
                    break;
                }
            }
        }

        data->processed = next;
    }

    svcCloseHandle(data->cancelEvent);

    data->finished = true;

    aptSetSleepAllowed(true);
}

Result task_data_op(data_op_data* data) {
    if(data == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    data->processed = 0;

    data->currProcessed = 0;
    data->currTotal = 0;

    data->finished = false;
    data->result = 0;
    data->cancelEvent = 0;

    Result res = 0;
    if(R_SUCCEEDED(res = svcCreateEvent(&data->cancelEvent, RESET_STICKY))) {
        if(threadCreate(task_data_op_thread, data, 0x10000, 0x18, 1, true) == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
        }
    }

    if(R_FAILED(res)) {
        data->finished = true;

        if(data->cancelEvent != 0) {
            svcCloseHandle(data->cancelEvent);
            data->cancelEvent = 0;
        }
    }

    aptSetSleepAllowed(false);

    return res;
}