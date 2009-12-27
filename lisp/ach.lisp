;; Copyright (c) 2009, Georgia Tech Research Corporation
;; All rights reserved.
;;
;; Redistribution and use in source and binary forms, with or without
;; modification, are permitted provided that the following conditions
;; are met:
;;
;; * Redistributions of source code must retain the above copyright
;;   notice, this list of conditions and the following disclaimer.
;;
;; * Redistributions in binary form must reproduce the above copyright
;;   notice, this list of conditions and the following disclaimer in
;;   the documentation and/or other materials provided with the
;;   distribution.
;;
;; * Neither the name of the copyright holder(s) nor the names of its
;;   contributors may be used to endorse or promote products derived
;;   from this software without specific prior written permission.
;;
;; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;; "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;; LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
;; FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
;; COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
;; INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
;; (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
;; SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
;; HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
;; STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
;; ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
;; OF THE POSSIBILITY OF SUCH DAMAGE.


;; Author: Neil T. Dantam



(defpackage :ach
  (:use :cl :binio)
  (:export :ach-open :ach-close :ach-read :ach-write :make-listener :make-ach-sync))

(in-package :ach)

;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; GENERAL INTERACTION ;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;

(defstruct channel
  direction
  process)

;; let's make the delimiters int32s.  That'll be faster, right?

(defparameter *size-delim* (decode-uint (map-into (make-octet-vector 4) #'char-code "size") :little))
(defparameter *data-delim* (decode-uint (map-into (make-octet-vector 4) #'char-code "data") :little))

(defparameter +next-cmd+ (sb-ext:string-to-octets "next"))
(defparameter +last-cmd+ (sb-ext:string-to-octets "last"))
;(defparameter +next-cmd+ "next")
;(defparameter +last-cmd+ "last")

(defun channel-input (channel)
  (sb-ext:process-input (channel-process channel)))

(defun channel-output (channel)
  (sb-ext:process-output (channel-process channel)))

(defun ach-open (channel-name &key (direction :input) last)
  "Open an ach channel"
  (let ((proc-input (when (or (eq direction :output)
                              (eq direction :input-sync))
                      :stream))
        (proc-output (when (or (eq direction :input)
                               (eq direction :input-sync))
                       :stream)))
    (make-channel
     :direction direction
     :process (sb-ext:run-program "achpipe"
                                  `(
                                    ,(case direction
                                           (:input "-s")
                                           (:input-sync "-S")
                                           (:output "-p")
                                           (otherwise (error "Invalid direction: ~A"
                                                             direction)))
                                     ,channel-name
                                     ,@(when last (list "--last")))
                                  :search t
                                  :wait nil
                                  :input proc-input
                                  ;;:input-element-type '(unsigned-byte 8)
                                  :output proc-output
                                  ;;:output-element-type '(unsigned-byte 8)
                                  :error *standard-output*))))
                                  ;;:error "/tmp/achlisperr"))))



;;(defun read-bytes-dammit (buffer stream)
;;  (declare (octet-vector buffer))
;;  (if (eq (stream-element-type stream) 'unsigned-byte)
;;      (read-sequence buffer stream)
;;      (dotimes (i (length buffer))
;;        (setf (aref buffer i)
;;              (read-byte stream)))))

(defun write-bytes-dammit (buffer stream)
  (declare (octet-vector buffer))
  (if (eq (stream-element-type stream) 'unsigned-byte)
      (write-sequence buffer stream)
      (dotimes (i (length buffer))
        (write-byte (aref buffer i) stream ))))

(defun make-size-buf ()
  (make-octet-vector 12))

(defun ach-read (channel &optional buffer)
  "read a frame from the channel"
  (declare (type (or null octet-vector) buffer))
  (assert (or (eq :input (channel-direction channel))
              (eq :input-sync (channel-direction channel)))
          () "Channel direction must be :INPUT, not ~S"
          (channel-direction channel))
  (let ((s (channel-output channel))
        ;;(size-buf (make-array 12 :element-type 'unsigned-byte)))
        (size-buf (make-size-buf)))
    (read-sequence size-buf s)
    (assert (= *size-delim*
               (decode-uint size-buf :little 0)) ()
               "Invalid size delimiter: ~A" (subseq size-buf 0 4))
    (assert (= *data-delim*
               (decode-uint size-buf :little 8)) ()
               "Invalid size delimiter: ~A" (subseq size-buf 8))
    (let* ((size (decode-uint size-buf :big 4))
           ;;(buffer (make-array size :element-type '(unsigned-byte 8))))
           (buffer (if (= size (length buffer)) buffer
                       (make-octet-vector size))))
      ;(read-bytes-dammit buffer s)
      (read-sequence buffer s)
      buffer)))



(defun ach-write (channel buffer)
  "write a frame to the channel"
  (declare (octet-vector buffer))
  (assert (eq :output (channel-direction channel)) ()
          "Channel direction must be :OUTPUT, not ~S"
          (channel-direction channel))
  (let ((s (channel-input channel))
        ;;(size-buf (make-array 12 :element-type 'unsigned-byte)))
        (size-buf (make-size-buf)))
    (encode-int *size-delim* :little 0)
    (encode-int (length buffer) :big 4)
    (encode-int *data-delim* :little 8)
    (write-sequence size-buf s)
    (write-sequence buffer s)))
    ;;(write-bytes-dammit size-buf s)
    ;;(write-bytes-dammit buffer s)))


(defun ach-status (channel)
 (sb-ext:process-status (channel-process channel)))

(defun ach-close (channel)
  "Close ach channel"
  (sb-ext:process-kill (channel-process channel) sb-posix:sigterm)
  (sb-ext:process-wait (channel-process channel) t)
  (sb-ext:process-close (channel-process channel))
  (setf (channel-direction channel) nil)
  channel)


;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; CONTINUOUS READING ;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;

;;; maintain a background thread that will continuously read the
;;; channel

(defun make-listener (channel-name &key last)
  ;;(declare (optimize (speed 3) (safety 0)))
  (let ((lock (sb-thread:make-mutex))
        (buffer (make-octet-vector 0))
        (continue t))
    (declare (octet-vector buffer))
    (let ((thread  ; start the listening thread
           (sb-thread:make-thread
            (lambda () (let ((channel (ach-open channel-name
                                           :direction :input :last last)))
                    (unwind-protect (loop
                                       while continue
                                       for tmp-buffer = (ach-read channel)
                                       do (sb-thread:with-mutex (lock)
                                            (setq buffer tmp-buffer)))
                      (ach-close channel))))
            :name (concatenate 'string "ach-listener-" channel-name))))
      ;; return a closure to get the next buffer or terminate the listener
      (lambda (&key terminate)
        (if terminate
            ;; stop the listener
            (progn
              ;; FIXME: be more robust
              (setq continue nil)
              (sb-thread:join-thread thread))
            ;; get the buffer
            (progn
              (sb-thread:with-mutex (lock)
                (copy-seq buffer))))))))


(defun send-sync-cmd (channel last)
  (write-sequence (if last +last-cmd+ +next-cmd+)
                  (channel-input channel))
  (finish-output (channel-input channel)))

(defun make-ach-sync (channel-name)
  (let ((channel (ach-open channel-name :direction :input-sync)))
    (let ((fun (lambda (&key (last t) terminate channel-hook)
                 (cond
                   (terminate
                    ;; close channel
                    (ach-close channel))
                   (channel-hook
                    (funcall channel-hook channel))
                   (t
                    ;; read channel
                    (progn
                      ;; send cmd
                      (send-sync-cmd channel last)
                      ;; get data
                      (ach-read channel)))))))
      (sb-ext:finalize fun (lambda () (ach-close channel)))
      fun)))

