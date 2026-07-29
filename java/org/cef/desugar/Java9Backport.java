package org.cef.desugar;

import java.util.AbstractList;
import java.util.AbstractMap;
import java.util.AbstractSet;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Set;

public final class Java9Backport {

    private Java9Backport() {}

    @SafeVarargs
    @SuppressWarnings("varargs")
    public static <E> List<E> listOf(E... elements) {
        Objects.requireNonNull(elements);
        switch (elements.length) {
            case 0:
                return Collections.emptyList();
            case 1:
                return Collections.singletonList(elements[0]);
            default:
                for (E e : elements) {
                    Objects.requireNonNull(e);
                }
                return Collections.unmodifiableList(new ArrayList<E>(Arrays.asList(elements)));
        }
    }

    public static <E> List<E> listCopyOf(Collection<? extends E> coll) {
        Objects.requireNonNull(coll);
        List<E> list = new ArrayList<E>(coll);
        for (E e : list) {
            Objects.requireNonNull(e);
        }
        return Collections.unmodifiableList(list);
    }

    @SafeVarargs
    @SuppressWarnings("varargs")
    public static <E> Set<E> setOf(E... elements) {
        Objects.requireNonNull(elements);
        switch (elements.length) {
            case 0:
                return Collections.emptySet();
            case 1:
                return Collections.singleton(Objects.requireNonNull(elements[0]));
            default:
                Set<E> set = new HashSet<E>(elements.length * 4 / 3 + 1);
                for (E e : elements) {
                    Objects.requireNonNull(e);
                    if (!set.add(e)) {
                        throw new IllegalArgumentException("duplicate element: " + e);
                    }
                }
                return Collections.unmodifiableSet(set);
        }
    }

    public static <E> Set<E> setCopyOf(Collection<? extends E> coll) {
        Objects.requireNonNull(coll);
        Set<E> set = new HashSet<E>(coll.size() * 4 / 3 + 1);
        for (E e : coll) {
            Objects.requireNonNull(e);
            if (!set.add(e)) {
                throw new IllegalArgumentException("duplicate element: " + e);
            }
        }
        return Collections.unmodifiableSet(set);
    }

    public static <K, V> Map<K, V> mapOf() {
        return Collections.emptyMap();
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1) {
        Objects.requireNonNull(k1);
        Objects.requireNonNull(v1);
        return Collections.singletonMap(k1, v1);
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2) {
        Objects.requireNonNull(k1);
        Objects.requireNonNull(v1);
        Objects.requireNonNull(k2);
        Objects.requireNonNull(v2);
        Map<K, V> map = new HashMap<K, V>(4);
        map.put(k1, v1);
        if (map.containsKey(k2)) {
            throw new IllegalArgumentException("duplicate key: " + k2);
        }
        map.put(k2, v2);
        return Collections.unmodifiableMap(map);
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3) {
        Objects.requireNonNull(k1);
        Objects.requireNonNull(v1);
        Objects.requireNonNull(k2);
        Objects.requireNonNull(v2);
        Objects.requireNonNull(k3);
        Objects.requireNonNull(v3);
        Map<K, V> map = new HashMap<K, V>(8);
        map.put(k1, v1);
        if (map.containsKey(k2)) {
            throw new IllegalArgumentException("duplicate key: " + k2);
        }
        map.put(k2, v2);
        if (map.containsKey(k3)) {
            throw new IllegalArgumentException("duplicate key: " + k3);
        }
        map.put(k3, v3);
        return Collections.unmodifiableMap(map);
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5, K k6, V v6) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5), entry(k6, v6));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5, K k6, V v6, K k7, V v7) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5), entry(k6, v6), entry(k7, v7));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5, K k6, V v6, K k7, V v7, K k8, V v8) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5), entry(k6, v6), entry(k7, v7), entry(k8, v8));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5, K k6, V v6, K k7, V v7, K k8, V v8, K k9, V v9) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5), entry(k6, v6), entry(k7, v7), entry(k8, v8), entry(k9, v9));
    }

    public static <K, V> Map<K, V> mapOf(K k1, V v1, K k2, V v2, K k3, V v3, K k4, V v4, K k5, V v5, K k6, V v6, K k7, V v7, K k8, V v8, K k9, V v9, K k10, V v10) {
        return mapOfEntries(
            entry(k1, v1), entry(k2, v2), entry(k3, v3), entry(k4, v4), entry(k5, v5), entry(k6, v6), entry(k7, v7), entry(k8, v8), entry(k9, v9), entry(k10, v10));
    }

    @SafeVarargs
    @SuppressWarnings({"unchecked", "varargs"})
    public static <K, V> Map<K, V> mapOfEntries(Map.Entry<? extends K, ? extends V>... entries) {
        Objects.requireNonNull(entries);
        Map<K, V> map = new HashMap<K, V>(entries.length * 4 / 3 + 1);
        for (Map.Entry<? extends K, ? extends V> entry : entries) {
            K k = Objects.requireNonNull(entry.getKey());
            V v = Objects.requireNonNull(entry.getValue());
            if (map.containsKey(k)) {
                throw new IllegalArgumentException("duplicate key: " + k);
            }
            map.put(k, v);
        }
        return Collections.unmodifiableMap(map);
    }

    public static <K, V> Map<K, V> mapCopyOf(Map<? extends K, ? extends V> map) {
        Objects.requireNonNull(map);
        Map<K, V> result = new HashMap<K, V>(map.size() * 4 / 3 + 1);
        for (Map.Entry<? extends K, ? extends V> e : map.entrySet()) {
            K k = Objects.requireNonNull(e.getKey());
            V v = Objects.requireNonNull(e.getValue());
            if (result.containsKey(k)) {
                throw new IllegalArgumentException("duplicate key: " + k);
            }
            result.put(k, v);
        }
        return Collections.unmodifiableMap(result);
    }

    public static <K, V> Map.Entry<K, V> entry(K k, V v) {
        Objects.requireNonNull(k);
        Objects.requireNonNull(v);
        return new AbstractMap.SimpleImmutableEntry<K, V>(k, v);
    }

    public static <T> T requireNonNullElse(T obj, T defaultObj) {
        return (obj != null) ? obj : Objects.requireNonNull(defaultObj);
    }

    public static <T> T requireNonNullElseGet(T obj, java.util.function.Supplier<? extends T> supplier) {
        if (obj != null) return obj;
        T t = supplier.get();
        return Objects.requireNonNull(t);
    }

    public static boolean stringIsBlank(String s) {
        if (s == null) throw new NullPointerException();
        int length = s.length();
        for (int i = 0; i < length; i++) {
            if (!Character.isWhitespace(s.charAt(i))) {
                return false;
            }
        }
        return true;
    }

    public static String stringRepeat(String s, int count) {
        if (s == null) throw new NullPointerException();
        if (count < 0) throw new IllegalArgumentException("count is negative: " + count);
        if (count == 1) return s;
        int len = s.length();
        if (len == 0 || count == 0) return "";
        if (Integer.MAX_VALUE / count < len) {
            throw new OutOfMemoryError("Required length exceeds implementation limit");
        }
        StringBuilder sb = new StringBuilder(len * count);
        for (int i = 0; i < count; i++) {
            sb.append(s);
        }
        return sb.toString();
    }

    public static String stringStrip(String s) {
        if (s == null) throw new NullPointerException();
        int len = s.length();
        int start = 0;
        int end = len;
        while (start < end && Character.isWhitespace(s.charAt(start))) {
            start++;
        }
        while (end > start && Character.isWhitespace(s.charAt(end - 1))) {
            end--;
        }
        return (start > 0 || end < len) ? s.substring(start, end) : s;
    }

    public static String stringStripLeading(String s) {
        if (s == null) throw new NullPointerException();
        int len = s.length();
        int start = 0;
        while (start < len && Character.isWhitespace(s.charAt(start))) {
            start++;
        }
        return start > 0 ? s.substring(start) : s;
    }

    public static String stringStripTrailing(String s) {
        if (s == null) throw new NullPointerException();
        int len = s.length();
        int end = len;
        while (end > 0 && Character.isWhitespace(s.charAt(end - 1))) {
            end--;
        }
        return end < len ? s.substring(0, end) : s;
    }
}