/****************************************************************************
 * sync/SyncPlan.cpp —— 对账决策实现（纯逻辑，无 IO）
 *
 * 判定骨架（三方比较）与安全约定见 SyncPlan.h 顶部说明。本文件只做决策，不碰磁盘与网络。
 ****************************************************************************/
#include "SyncPlan.h"

#include <QStringList>

#include <algorithm>

namespace cv {

// ============================ SyncSnapshot ============================

bool SyncSnapshot::Item::sameContentAs(const Item &o) const
{
    if (isDir != o.isDir)
        return false;
    if (isDir)
        return true;   // 目录没有"内容"，只比是不是目录

    if (size != o.size)
        return false;
    // ① 两侧都有内容哈希 → 这是最可靠的判据（客户端算整文件 SHA-256，与服务端 content_hash 同口径）
    if (!hash.isEmpty() && !o.hash.isEmpty())
        return hash == o.hash;
    // ② 退而用云端版本号：跨端本地 mtime 与云端 created_at 本就不可比，rev 才是云端权威信号
    if (!rev.isEmpty() && !o.rev.isEmpty())
        return rev == o.rev;
    // ③ 都没有 → 只能比时间（精度/时区都可能不一致，属兜底）
    return mtime == o.mtime;
}

const SyncSnapshot::Item *SyncSnapshot::find(const QString &relPath) const
{
    auto it = items.constFind(relPath);
    return it == items.constEnd() ? nullptr : &it.value();
}

namespace {

bool allowsUpload(SyncMode m) { return m != SyncMode::DownloadOnly; }
bool allowsDownload(SyncMode m) { return m != SyncMode::UploadOnly; }

QString baseName(const QString &relPath)
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? relPath : relPath.mid(slash + 1);
}

QString dirNameOf(const QString &relPath)
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : relPath.left(slash);
}

// 被"选择性同步"排除时应当降级为 Skip 的动作类型。
// 各类的取舍理由：
//   · DownloadNew / DownloadUpdate / MakeLocalDir —— 核心语义：不落地本地。
//   · ConflictKeepRemote —— 以云端为准下载，同样要拦（否则等于偷偷下载了）。
//   · DeleteLocal —— 不删本地：目录可能是"刚被排除"的，本地还留着之前下过的副本，
//     因一次云端删除就把用户本地的东西删掉太危险（排除的语义是"云端保留"，不是"清本地"）。
//   · DeleteRemote —— 也不动云端：用户明确说该目录"仅云端保留"，
//     就不该因为本地没这份而把云端的删掉。
// 反过来说，**上传方向不受影响**：排除的是"云端→本地"，不是"本地→云端"。
bool blockedWhenExcluded(SyncAction::Kind k) {
  switch (k) {
    case SyncAction::DownloadNew:
    case SyncAction::DownloadUpdate:
    case SyncAction::MakeLocalDir:
    case SyncAction::ConflictKeepRemote:
    case SyncAction::DeleteLocal:
    case SyncAction::DeleteRemote:
      return true;
    default:
      return false;
  }
}

const char* kindName(SyncAction::Kind k) {
  switch (k) {
    case SyncAction::UploadNew:          return "上传新增";
    case SyncAction::UploadUpdate:       return "上传修改";
    case SyncAction::DownloadNew:        return "下载新增";
    case SyncAction::DownloadUpdate:     return "下载修改";
    case SyncAction::DeleteRemote:       return "删除云端";
    case SyncAction::DeleteLocal:        return "删除本地";
    case SyncAction::ConflictKeepLocal:  return "冲突·保留本地";
    case SyncAction::ConflictKeepRemote: return "冲突·保留云端";
    case SyncAction::MakeLocalDir:       return "建本地目录";
    case SyncAction::MakeRemoteDir:      return "建云端目录";
    case SyncAction::Skip:               return "跳过";
  }
  return "?";
}

QString modeNote(SyncMode m)
{
    switch (m) {
    case SyncMode::UploadOnly:   return QStringLiteral("仅上传模式");
    case SyncMode::DownloadOnly: return QStringLiteral("仅下载模式");
    default:                     return QStringLiteral("双向模式");
    }
}

} // namespace

// ============================ 冲突副本命名 ============================

bool SyncPlan::isExcluded(const QStringList &excludeDirs, const QString &relPath) {
  if (excludeDirs.isEmpty() || relPath.isEmpty()) return false;
  for (const QString &raw : excludeDirs) {
    QString e = raw.trimmed();
    while (e.startsWith(QLatin1Char('/'))) e.remove(0, 1);   // 允许用户写 "/照片"
    while (e.endsWith(QLatin1Char('/'))) e.chop(1);          // 也允许写 "照片/"
    if (e.isEmpty()) continue;                               // 空条目忽略（不是"排除全部"！）
    if (relPath == e) return true;
    // ⚠️ 必须带 '/' 一起比：若写成 relPath.startsWith(e)，
    //    排除 "docs" 会连 "docs2"、"docs.txt" 一起误伤 —— 用户的文件会莫名其妙不再同步。
    if (relPath.startsWith(e + QLatin1Char('/'))) return true;
  }
  return false;
}

QString SyncPlan::conflictName(const QString &name, const QString &device, const QDateTime &when)
{
    // 拆扩展名：`报告.docx` → stem=报告 ext=.docx；`归档.tar.gz` → stem=归档.tar ext=.gz
    // （按最后一个点切，与 git / Windows 的习惯一致）
    QString stem = name;
    QString ext;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        stem = name.left(dot);
        ext  = name.mid(dot);
    }

    QString dev = device.trimmed();
    if (dev.isEmpty())
        dev = QStringLiteral("未知设备");
    // Windows 文件名非法字符一律换成 '_'，否则冲突副本可能根本创建不出来
    const QString kBad = QStringLiteral("\\/:*?\"<>|");
    for (int i = 0; i < dev.size(); ++i) {
        if (kBad.contains(dev.at(i)))
            dev[i] = QLatin1Char('_');
    }

    const QDateTime localWhen = when.isValid() ? when.toLocalTime() : QDateTime::currentDateTime();
    const QString     stamp   = localWhen.toString(QStringLiteral("yyyyMMdd-HHmmss"));
    return QStringLiteral("%1(冲突-%2-%3)%4").arg(stem, dev, stamp, ext);
}

// ============================ 决策主体 ============================

SyncPlanResult SyncPlan::build(const SyncPlanInput &in)
{
    SyncPlanResult out;

    const bool up   = allowsUpload(in.mode);
    const bool down = allowsDownload(in.mode);

    // ---- 收集三方路径并集，并**排序**：QSet/QHash 迭代无序，排序后输出才可复现（日志与测试都需要）----
    QStringList paths;
    {
        const auto collect = [&paths](const SyncSnapshot &s) {
            for (auto it = s.items.constBegin(); it != s.items.constEnd(); ++it) {
                if (!paths.contains(it.key()))
                    paths << it.key();
            }
        };
        collect(in.lastSynced);
        collect(in.local);
        collect(in.remote);
    }
    std::sort(paths.begin(), paths.end());

    // 目录动作与文件动作分开收集，最后把目录排到前面 —— 否则"先写文件后建目录"必然失败
    QVector<SyncAction> dirActions;
    QVector<SyncAction> fileActions;

    const auto addAction = [&](SyncAction::Kind kindIn, const QString &p, bool isDir,
                               const QString &reasonIn, const QString &conflictPath = QString()) {
        // 选择性同步（FR-3-7）**单一收口**：落在被排除云端目录下的动作一律降级为 Skip。
        // 不放在各分支里各判一次 —— 漏一个分支就会把"仅云端保留"的目录拉到本地，
        // 或者删掉用户明确要留在云端的数据，而这类漏洞靠人工审查很难穷尽。
        SyncAction::Kind kind   = kindIn;
        QString          reason = reasonIn;
        if (kind != SyncAction::Skip && blockedWhenExcluded(kind) &&
            SyncPlan::isExcluded(in.excludeDirs, p)) {
            reason = QStringLiteral("已在「选择性同步」中排除（仅云端保留）；原动作：%1")
                         .arg(QString::fromUtf8(kindName(kind)));
            kind = SyncAction::Skip;
        }

        SyncAction a;
        a.kind         = kind;
        a.relPath      = p;
        a.conflictPath = (kind == SyncAction::Skip) ? QString() : conflictPath;
        a.reason       = reason;

        // 计数（Skip 也计，便于日志说明"为什么什么都没做"）
        switch (kind) {
        case SyncAction::UploadNew:
        case SyncAction::UploadUpdate:      ++out.counts.up; break;
        case SyncAction::DownloadNew:
        case SyncAction::DownloadUpdate:    ++out.counts.down; break;
        case SyncAction::DeleteLocal:       ++out.counts.delLocal; break;
        case SyncAction::DeleteRemote:      ++out.counts.delRemote; break;
        case SyncAction::ConflictKeepLocal:
        case SyncAction::ConflictKeepRemote:++out.counts.conflicts; break;
        case SyncAction::MakeLocalDir:
        case SyncAction::MakeRemoteDir:     ++out.counts.dirs; break;
        case SyncAction::Skip:              ++out.counts.skipped; break;
        }

        const bool isDirAction = (kind == SyncAction::MakeLocalDir || kind == SyncAction::MakeRemoteDir)
                                 || ((kind == SyncAction::DeleteLocal || kind == SyncAction::DeleteRemote)
                                     && isDir);
        if (isDirAction)
            dirActions.push_back(a);
        else
            fileActions.push_back(a);
    };

    const auto skip = [&](const QString &p, SyncMode m, const QString &why) {
        addAction(SyncAction::Skip, p, false,
                  QStringLiteral("%1：%2").arg(modeNote(m), why));
    };

    for (const QString &p : paths) {
        const SyncSnapshot::Item *base = in.lastSynced.find(p);
        const SyncSnapshot::Item *loc  = in.local.find(p);
        const SyncSnapshot::Item *rem  = in.remote.find(p);

        const bool isDir = loc ? loc->isDir : (rem ? rem->isDir : (base ? base->isDir : false));

        // 生成冲突副本的本地相对路径（与原文件同目录）
        const auto conflictPathFor = [&]() {
            const QString cp = conflictName(baseName(p), in.deviceName, in.now);
            const QString d  = dirNameOf(p);
            return d.isEmpty() ? cp : (d + QLatin1Char('/') + cp);
        };

        // 冲突：按"较新者为主、较旧者另存"处理；类型冲突（目录 vs 文件）不自动合并
        const auto resolveConflict = [&](const SyncSnapshot::Item &l, const SyncSnapshot::Item &r,
                                         const char *why) {
            if (l.isDir != r.isDir) {
                // 一端目录一端文件：自动"解决"很可能毁掉一整棵子树，改为显式跳过 + 记为冲突
                addAction(SyncAction::Skip, p, isDir,
                          QStringLiteral("%1：类型冲突（一端是目录、一端是文件），需人工处理：%2")
                              .arg(modeNote(in.mode), QString::fromUtf8(why)));
                ++out.counts.conflicts;
                return;
            }
            if (in.mode == SyncMode::UploadOnly) {
                addAction(SyncAction::ConflictKeepLocal, p, false,
                          QStringLiteral("仅上传模式：以本地为准上传，云端旧内容不保留（%1）")
                              .arg(QString::fromUtf8(why)));
                return;
            }
            if (in.mode == SyncMode::DownloadOnly) {
                addAction(SyncAction::ConflictKeepRemote, p, false,
                          QStringLiteral("仅下载模式：以云端为准，本地旧内容另存为冲突副本（%1）")
                              .arg(QString::fromUtf8(why)));
                return;
            }
            // 双向：较新者为主。mtime 不可比时保守地把本地当较新（本地为主 + 云端留副本，两份都不丢）
            const bool localNewer = (l.mtime.isValid() && r.mtime.isValid())
                                        ? (l.mtime > r.mtime)
                                        : true;
            if (localNewer) {
                addAction(SyncAction::ConflictKeepLocal, p, false,
                          QStringLiteral("两端都改且本地较新：本地为主，云端旧内容另存冲突副本（%1）")
                              .arg(QString::fromUtf8(why)),
                          conflictPathFor());
            } else {
                addAction(SyncAction::ConflictKeepRemote, p, false,
                          QStringLiteral("两端都改且云端较新：云端为主，本地旧内容另存冲突副本（%1）")
                              .arg(QString::fromUtf8(why)),
                          conflictPathFor());
            }
        };

        // ---------------------------------------------------------------
        // ① 无基准（新出现的东西）
        // ---------------------------------------------------------------
        if (!base) {
            if (loc && rem) {
                if (loc->sameContentAs(*rem)) {
                    addAction(SyncAction::Skip, p, false, QStringLiteral("两端均为新增且内容一致"));
                } else {
                    resolveConflict(*loc, *rem, "两端各自新增");
                }
                continue;
            }
            if (loc && !rem) {
                if (!up) {
                    skip(p, in.mode, QStringLiteral("忽略本地新增"));
                    continue;
                }
                addAction(loc->isDir ? SyncAction::MakeRemoteDir : SyncAction::UploadNew, p,
                          loc->isDir,
                          loc->isDir ? QStringLiteral("本地新增目录") : QStringLiteral("本地新增文件"));
                continue;
            }
            if (!loc && rem) {
                if (!down) {
                    skip(p, in.mode, QStringLiteral("忽略云端新增"));
                    continue;
                }
                addAction(rem->isDir ? SyncAction::MakeLocalDir : SyncAction::DownloadNew, p,
                          rem->isDir,
                          rem->isDir ? QStringLiteral("云端新增目录") : QStringLiteral("云端新增文件"));
                continue;
            }
            continue;   // base 存在而 loc/rem 皆无 → 不可能
        }

        // ---------------------------------------------------------------
        // ② 有基准：判断哪一侧真的变了
        // ---------------------------------------------------------------
        const bool locGone    = (loc == nullptr);
        const bool remGone    = (rem == nullptr);
        const bool locChanged = loc && !loc->sameContentAs(*base);
        const bool remChanged = rem && !rem->sameContentAs(*base);

        if (locGone && remGone) {
            addAction(SyncAction::Skip, p, isDir, QStringLiteral("两端均已删除"));
            continue;
        }

        // 本地被删
        if (locGone) {
            if (remChanged) {
                // 本地删除期间云端又改了 → 不能按删除处理（会把云端新内容删掉）
                if (!down) {
                    skip(p, in.mode, QStringLiteral("本地已删但云端有更新，本模式不下载"));
                    continue;
                }
                addAction(rem->isDir ? SyncAction::MakeLocalDir : SyncAction::DownloadUpdate, p,
                          rem->isDir,
                          QStringLiteral("本地已删但云端有更新 → 以云端为准恢复（避免丢失云端改动）"));
                continue;
            }
            if (!in.deleteSyncEnabled) {
                addAction(SyncAction::Skip, p, isDir,
                          QStringLiteral("删除同步已关闭：仅解除同步，云端保留"));
                continue;
            }
            if (!up) {
                skip(p, in.mode, QStringLiteral("不把本地删除传播到云端"));
                continue;
            }
            addAction(SyncAction::DeleteRemote, p, isDir,
                      QStringLiteral("本地已删除 → 删除云端（进回收站）"));
            continue;
        }

        // 云端被删
        if (remGone) {
            if (locChanged) {
                // 云端删除期间本地又改了 → 以本地为准重新上传，绝不静默丢弃本地改动
                if (!up) {
                    skip(p, in.mode, QStringLiteral("云端已删但本地有更新，本模式不上传"));
                    continue;
                }
                addAction(loc->isDir ? SyncAction::MakeRemoteDir : SyncAction::UploadUpdate, p,
                          loc->isDir,
                          QStringLiteral("云端已删但本地有更新 → 以本地为准重新上传（避免丢失本地改动）"));
                continue;
            }
            if (!in.deleteSyncEnabled) {
                addAction(SyncAction::Skip, p, isDir,
                          QStringLiteral("删除同步已关闭：仅解除同步，本地保留"));
                continue;
            }
            if (!down) {
                skip(p, in.mode, QStringLiteral("不把云端删除传播到本地"));
                continue;
            }
            addAction(SyncAction::DeleteLocal, p, isDir,
                      QStringLiteral("云端已删除 → 删除本地"));
            continue;
        }

        // ---------------------------------------------------------------
        // ③ 两端都在
        // ---------------------------------------------------------------
        if (locChanged && !remChanged) {
            if (!up) {
                skip(p, in.mode, QStringLiteral("忽略本地改动"));
                continue;
            }
            addAction(loc->isDir ? SyncAction::MakeRemoteDir : SyncAction::UploadUpdate, p,
                      loc->isDir, QStringLiteral("本地已修改 → 上传"));
            continue;
        }
        if (!locChanged && remChanged) {
            if (!down) {
                skip(p, in.mode, QStringLiteral("忽略云端改动"));
                continue;
            }
            addAction(rem->isDir ? SyncAction::MakeLocalDir : SyncAction::DownloadUpdate, p,
                      rem->isDir, QStringLiteral("云端已修改 → 下载"));
            continue;
        }
        if (locChanged && remChanged) {
            if (loc->sameContentAs(*rem)) {
                addAction(SyncAction::Skip, p, false,
                          QStringLiteral("两端各自修改但结果一致（无需传内容）"));
            } else {
                resolveConflict(*loc, *rem, "两端都改");
            }
            continue;
        }

        addAction(SyncAction::Skip, p, isDir, QStringLiteral("与上次同步状态一致，无需处理"));
    }

    // 目录动作前置：否则"先写文件、后建目录"必然失败
    out.actions = dirActions;
    out.actions += fileActions;
    return out;
}

} // namespace cv
