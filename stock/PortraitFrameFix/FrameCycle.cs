using System;
using System.Collections;

namespace KerbalOverEngineering.Portraits
{
    internal static class FrameCycle
    {
        internal static IEnumerator Run(IEnumerator original, Func<bool> alive, Func<bool> visible, Func<object, bool> timedWait)
        {
            try
            {
                while (alive())
                {
                    if (!visible())
                    {
                        yield return null;
                        continue;
                    }

                    if (!original.MoveNext())
                        yield break;
                    object wait = original.Current;
                    yield return timedWait(wait) ? null : wait;
                }
            }
            finally
            {
                IDisposable disposable = original as IDisposable;
                if (disposable != null)
                    disposable.Dispose();
            }
        }
    }
}
