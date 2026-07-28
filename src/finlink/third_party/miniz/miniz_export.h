#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Hand-written stand-in for the file CMake's GenerateExportHeader module
 * generates in upstream miniz. We only ever link finlink_core statically
 * (across host, Android, 3DS, Switch, ...), so no dllexport/visibility
 * dance is needed. */

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED
#define MINIZ_DEPRECATED_EXPORT
#define MINIZ_DEPRECATED_NO_EXPORT

#endif /* MINIZ_EXPORT_H */
