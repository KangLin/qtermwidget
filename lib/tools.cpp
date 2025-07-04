#include "tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QtDebug>

Q_LOGGING_CATEGORY(qtermwidgetLogger, "qtermwidget", QtWarningMsg)

static QStringList custom_kb_layout_dirs;

void add_custom_kb_layout_dir(const QString& custom_dir)
{
    if (!custom_kb_layout_dirs.contains(custom_dir))
        custom_kb_layout_dirs << custom_dir;
}

/*! Helper function to get possible location of layout files.
By default the KB_LAYOUT_DIR is used (linux/BSD/macports).
But in some cases (apple bundle) there can be more locations).
*/
QString get_kb_layout_dir()
{
//    qDebug() << __FILE__ << __FUNCTION__;

    QString rval = QString();
    QString k(QLatin1String(KB_LAYOUT_DIR));
    QDir d(k);
    QString szPath;
    //qDebug() << "default KB_LAYOUT_DIR: " << k;

    if (d.exists())
    {
        rval = k.append(QLatin1Char('/'));
        return rval;
    }

    // subdir in the app location
    k = QCoreApplication::applicationDirPath()
        + QDir::separator() + QLatin1String("kb-layouts") + QDir::separator();
    d.setPath(k);
    //qDebug() << d.path();
    if (d.exists())
        return k;

    k = QCoreApplication::applicationDirPath()
        + QDir::separator() + QLatin1String("..") + QDir::separator()
        + QLatin1String("Resources") + QDir::separator() + QLatin1String("kb-layouts") + QDir::separator();
    d.setPath(k);
    if (d.exists())
        return k;

    k = QCoreApplication::applicationDirPath()
        + QDir::separator() + QLatin1String("..")
        + QDir::separator() + QLatin1String("share")
        + QDir::separator() + QLatin1String("qtermwidget6")
        + QDir::separator() + QLatin1String("kb-layouts") + QDir::separator();
    d.setPath(k);
    if(d.exists())
        return k;

    for (const QString& custom_dir : std::as_const(custom_kb_layout_dirs))
    {
        d.setPath(custom_dir);
        if (d.exists())
            return custom_dir + QDir::separator();
    }

    //qDebug() << "Cannot find KB_LAYOUT_DIR. Default:" << k;
    return QString();
}

/*! Helper function to add custom location of color schemes.
*/
static QStringList custom_color_schemes_dirs;
void add_custom_color_scheme_dir(const QString& custom_dir)
{
    if (!custom_color_schemes_dirs.contains(custom_dir))
        custom_color_schemes_dirs << custom_dir;
}

/*! Helper function to get possible locations of color schemes.
By default the COLORSCHEMES_DIR is used (linux/BSD/macports).
But in some cases (apple bundle) there can be more locations).
*/
const QStringList get_color_schemes_dirs()
{
//    qDebug() << __FILE__ << __FUNCTION__;

    QStringList rval;
    QString k(QLatin1String(COLORSCHEMES_DIR));
    QDir d(k);

//    qDebug() << "default COLORSCHEMES_DIR: " << k;

    if (d.exists())
        rval << k.append(QLatin1Char('/'));

    QString szPath;
    szPath = QCoreApplication::applicationDirPath() + QLatin1String("/../share/qtermwidget6/color-schemes/");
    d.setPath(szPath);
    //qDebug() << d.path();
    if (d.exists())
    {
        if (!rval.isEmpty())
            rval.clear();
        rval << szPath;
    }

#if defined(Q_OS_WIN)
    // subdir in the app location
    szPath = QCoreApplication::applicationDirPath() + QLatin1String("/color-schemes/");
    d.setPath(szPath);
    //qDebug() << d.path();
    if (d.exists())
    {
        if (!rval.isEmpty())
            rval.clear();
        rval << szPath;
    }
#endif
#ifdef Q_OS_MAC
    szPath = QCoreApplication::applicationDirPath() + QLatin1String("/../Resources/color-schemes/");
    d.setPath(szPath);
    if (d.exists())
    {
        if (!rval.isEmpty())
            rval.clear();
        rval << szPath;
    }
#endif

    for (const QString& custom_dir : std::as_const(custom_color_schemes_dirs))
    {
        d.setPath(custom_dir);
        if (d.exists())
            rval << custom_dir;
    }
#ifdef QT_DEBUG
    if(!rval.isEmpty()) {
        qDebug() << "Using color-schemes: " << rval;
    } else {
        qDebug() << "Cannot find color-schemes in any location!";
    }
#endif
    return rval;
}
