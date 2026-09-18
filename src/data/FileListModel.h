/****************************************************************************
 * data/FileListModel.h —— 文件列表数据模型（QML ListView 数据源）
 *
 * 冻结契约：docs/整合实现契约.md §4 —— 角色名必须严格一致：
 *     name  size  time  status  id  dir
 * 另附原始值 / 辅助角色（sizeBytes mtime hash），供界面按需使用（如 tooltip
 * 显示精确字节、双击预览取 hash）——契约要求为「至少」，追加角色不构成偏离。
 *
 * 设计要点：
 *   1. 优先 QAbstractListModel（QML 侧绑定最简单）；
 *   2. 排序在模型内部完成（QML 不排序），行为对齐已验收母体
 *      client/src/MainWindow.cpp::applySort 的 std::stable_sort 比较器；
 *   3. 只在主线程读写，不引入 QMutex；模型内不做任何网络 / IO。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

#include "Types.h"

namespace cv {

// 列表行 —— 轻量行结构。
//
// 为什么不直接复用 core/Types.h 的 FileItem：
//   服务端列表接口返回的是「扁平行」语义（id / name / size / dir / created_at /
//   hash / instant），FileItem 的 parentId / rev / version / trashed 等字段在本层
//   用不到，强塞会引入无谓的默认值歧义。字段取自母体 MainWindow.cpp 的 RowData，
//   保证与已验证逻辑一一对应；角色名不受影响。
struct FileRow {
    QString id;            // 唯一 ID（服务端字符串化）
    QString name;          // 文件 / 文件夹名
    QString dir;           // 所属目录（'' = 根目录，服务端用路径字符串）
    QString hash;          // SHA-256（可选，供预览 / 去重）
    QString status;        // 状态文案（如秒传："是"/"否"/"-"）
    qint64  size = 0;      // 字节（目录为 0）
    qint64  createdAt = 0; // 创建 / 修改时间（服务端 Unix 时间，毫秒或秒）

    // ---- 展示格式化（只读，不复用母体的非成员函数）----
    QString sizeText() const; // 人类可读大小，如 "1.23 MB"
    QString timeText() const; // "yyyy-MM-dd HH:mm:ss"，缺失显示 "-"
};

// 文件列表模型。
class FileListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // 角色枚举：前 6 个对应契约冻结角色名，其余为辅助角色。
    enum Roles {
        IdRole = Qt::UserRole + 1, // "id"
        NameRole,                  // "name"
        SizeRole,                  // "size"（展示字符串）
        SizeBytesRole,             // "sizeBytes"（原始字节，qint64）
        TimeRole,                  // "time"（展示字符串）
        MtimeRole,                 // "mtime"（原始时间，qint64）
        StatusRole,                // "status"
        DirRole,                   // "dir"
        HashRole                   // "hash"（辅助）
    };
    Q_ENUM(Roles)

    explicit FileListModel(QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 整批替换数据源（beginResetModel / endResetModel），替换后按当前排序键重排。
    void setRows(const QList<FileRow> &rows);

    // 稳定排序：key 0=名称 1=大小 2=时间；asc=true 升序 / false 降序。
    // 与 client/src/MainWindow.cpp::applySort 语义完全一致（含名称同值时按 dir 兜底）。
    Q_INVOKABLE void sortBy(int key, bool asc);

    // 供右键菜单读取某行的关键字段（越界返回空 map）。
    // 返回键：id / name / dir / size / sizeBytes / time / status / hash。
    Q_INVOKABLE QVariantMap rowAt(int row) const;

    // 清空数据源。
    Q_INVOKABLE void clear();

    // 行数（供 QML `model.count` 绑定）。
    int count() const;

signals:
    void countChanged();

private:
    // 内部条目：行数据 + 预计算的展示字符串。
    // QML ListView 会对可见项反复调用 data()，缓存展示串可避免重复格式化。
    struct Item {
        FileRow row;
        QString sizeText;
        QString timeText;
    };

    // 按 m_sortKey / m_sortAsc 对 m_items 做稳定排序（不改动模型信号状态）。
    void sortItems();

    QList<Item> m_items;
    int  m_sortKey = 2;      // 默认排序键：时间（与母体 m_sortKey 默认值一致）
    bool m_sortAsc = false;  // 默认降序（与母体 m_sortAsc 默认值一致）
};

} // namespace cv
