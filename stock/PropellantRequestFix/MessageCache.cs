using System;
using System.Collections.Generic;

namespace PropellantRequestFix
{
    // Cache presentation only. Resource availability and engine state never enter this cache.
    public sealed class MessageCache
    {
        const int Limit = 128;
        readonly Dictionary<string, string> messages = new Dictionary<string, string>(StringComparer.Ordinal);
        object owner, tags, culture, uiCulture;
        string language;
        int version;

        public void Observe(object nextOwner, object nextTags, int nextVersion, string nextLanguage,
            object nextCulture, object nextUiCulture)
        {
            if (ReferenceEquals(owner, nextOwner) && ReferenceEquals(tags, nextTags) && version == nextVersion &&
                language == nextLanguage && ReferenceEquals(culture, nextCulture) && ReferenceEquals(uiCulture, nextUiCulture)) return;
            messages.Clear();
            owner = nextOwner; tags = nextTags; version = nextVersion; language = nextLanguage;
            culture = nextCulture; uiCulture = nextUiCulture;
        }

        public string Get(string resource, Func<string, string> format)
        {
            // Null preserves the stock formatter's behavior/exception, without a Dictionary null-key exception.
            if (resource == null) return format(resource);
            string result;
            if (messages.TryGetValue(resource, out result)) return result;
            result = format(resource); // Exceptions must not poison the cache.
            if (messages.Count == Limit) messages.Clear();
            messages.Add(resource, result);
            return result;
        }
    }
}
