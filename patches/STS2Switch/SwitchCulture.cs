using System;
using System.Globalization;
using System.Threading;

namespace STS2Switch;

/// <summary>
/// Forces a real "en-US" culture instead of the invariant one.
///
/// The payload is built with InvariantGlobalization, so CurrentCulture is the
/// invariant culture, whose TwoLetterISOLanguageName is <c>"iv"</c>. SmartFormat
/// keys its pluralisation and number delegates by language and has no entry for
/// it, so run 24 produced this for every formatted string:
///
/// <code>
/// Localization formatting error! message=Error parsing format string:
/// IsoLangToDelegate not found for iv (Parameter 'twoLetterIsoLanguageName')
/// Receive [blue]{DamageIncrease:percentMore()}%[/blue] more damage from Attacks
/// for [blue]{Amount}[/blue] {Amount:plural:turn|turns}.
/// </code>
///
/// Every power description, card text and tooltip containing a plural or a
/// number formatter fails to render. That is the on-screen text corruption.
///
/// This works only because <c>PredefinedCulturesOnly=false</c> is set in
/// sts2.aot.props (added in run 11 for a different reason -- a
/// CultureNotFoundException in LocManager). Without it <c>new CultureInfo("en-US")</c>
/// throws in invariant mode rather than degrading. With it, the object exists,
/// reports "en", and behaves invariantly for actual collation and formatting --
/// which is exactly right for a console with no ICU: SmartFormat gets a language
/// it recognises, and nothing pretends to have locale data it does not have.
/// </summary>
public static class SwitchCulture
{
    private static int _done;

    /// <summary>
    /// Idempotent and cheap, so it can be called from several early entry
    /// points without coordinating which one runs first. The culture has to be
    /// set before LocManager's constructor, and the weave does not give a
    /// reliable "before everything" hook.
    /// </summary>
    public static void Ensure()
    {
        if (Interlocked.Exchange(ref _done, 1) != 0) return;

        try
        {
            var en = new CultureInfo("en-US");
            CultureInfo.DefaultThreadCurrentCulture = en;
            CultureInfo.DefaultThreadCurrentUICulture = en;
            CultureInfo.CurrentCulture = en;
            CultureInfo.CurrentUICulture = en;

            // Report whether this actually achieved anything, rather than
            // assuming it did.
            //
            // It does not. With InvariantGlobalization=true every CultureInfo
            // is invariant-backed, so TwoLetterISOLanguageName is "iv" whatever
            // name it is constructed with -- run 38 printed
            // "culture set to en-US (two-letter 'iv')", which is the disproof
            // of the fix sitting inside the fix's own log line.
            //
            // Name and formatting do change, so this is not useless: number and
            // date formatting now follow en-US conventions instead of invariant
            // ones. But SmartFormat keys its plural rules on the two-letter
            // code, and that stays "iv" until either ICU is shipped
            // (InvariantGlobalization=false, tens of MB) or SmartFormat's
            // lookup is patched to accept it.
            var two = en.TwoLetterISOLanguageName;
            if (two == "iv")
                SwitchLog.Info($"culture set to {en.Name} (two-letter 'iv', PluralRulePatches active)");
            else
                SwitchLog.Info($"culture set to {en.Name} (two-letter '{two}')");
        }
        catch (Exception ex)
        {
            // Not fatal. Without it, formatted strings stay broken but the game
            // runs -- which is what run 24 did for twenty minutes.
            SwitchLog.Error("could not set culture: " + ex.Message);
        }

        try
        {
            // Keep a fixed pool of 4 .NET PortableThreadPool worker threads alive
            // for the entire session. On libnx, pthread_detach is ENOSYS and un-joined
            // exiting threads leak 1 MB stack + 64 KB TLS + 1 Horizon TLS slot each;
            // disabling the 20s worker idle timeout eliminates both thread-exit leaks
            // (crash 01795290236 after 50 min) and mid-gameplay pthread_create churn.
            AppContext.SetData("System.Threading.ThreadPool.ThreadPoolThreadTimeoutMs", -1);
            AppContext.SetData("System.Threading.ThreadPool.ThreadsToKeepAlive", -1);
            ThreadPool.SetMinThreads(4, 4);
            ThreadPool.SetMaxThreads(4, 4);
            SwitchLog.Info("ThreadPool pinned to 4 persistent worker threads (timeout=-1)");
        }
        catch
        {
        }
    }
}
