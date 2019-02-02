﻿using System;
using System.Collections.Generic;
using System.Reflection;

namespace TinyTemplates
{
    class TemplateMemberAccessor
    {
        readonly IEnumerable<string> _memberPath;

        public TemplateMemberAccessor(IEnumerable<string> memberPath)
        {
            _memberPath = memberPath;
        }

        public object GetMember(Stack<object> model)
        {
            var r = model.Peek();
            foreach (var memberName in _memberPath)
            {
                var mi = r.GetType().GetTypeInfo().GetProperty(memberName, BindingFlags.IgnoreCase | BindingFlags.Instance | BindingFlags.Public | BindingFlags.GetProperty);
                if (mi == null)
                    throw new ArgumentException(string.Format("The property '{0}' does not exist.", memberName));
                r = mi.GetValue(r, null);
            }

            return r;
        }
    }
}